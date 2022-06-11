#include "thread.h"
#include "debug.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "memory.h"
#include "print.h"
#include "interrupt.h"

#define PG_SIZE 4096

struct task_struct* main_thread; // 主线程PCB
struct list thread_ready_list; // 就绪队列
struct list thread_all_list; // 所有任务队列
static struct list_elem* thread_tag;

// 下面这个函数是用汇编写的，现在这里声明extern，以后链接时再将汇编的函数写到可执行文件中
extern void switch_to(struct task_struct* cur, struct task_struct* next);

// 获取当前线程的PCB执政
struct task_struct* running_thread() {
    uint32_t esp;
    //内联汇编得到esp寄存器当前的值，存储在c语言变量esp中
    // = 表示只写， g表示可以存放到任意地点 ，圆括号中的表示c语言中的变量，表示将汇编结果放到c语言的这个变量中
    asm ("mov %%esp, %0" : "=g" (esp));
    // 因为PCB是按页分配的，所以 屏蔽esp的低12即可得到PCB的起始虚拟地址
    return (struct task_struct*) (esp & 0xfffff000);
} 
// 由kernel_thread 取执行function(arg)
static void kernel_thread(thread_func* function, void* func_arg) {
    // 进入中断处理器会自动关闭中断，在执行function前要开中断
    intr_enable();//执行前要开中断，避免时钟中断被屏蔽，否则操作系统就再也能重新接管整个系统了，function函数将会独占整个操作系统
    function(func_arg);
}

void thread_create(struct task_struct* pthread, thread_func function, void* func_arg) {
    // 在内核栈中预留站短使用的栈空间
    pthread->self_kstack -= sizeof(struct intr_stack);

    //再将self_kstack移动到线程栈的栈顶,这样self_kstack就名正言顺地成为了栈顶指针（低地址）
    pthread->self_kstack -= sizeof(struct task_struct);
    //将self_kstack的地址解释为kthread_stack 指针，以便后续的赋值
    struct thread_stack* kthread_stack = (struct thread_stack*)pthread->self_kstack;
    //下一行赋值是最难懂的
    kthread_stack->eip = kernel_thread;//eip是个函数指针，在这里被赋值为kernel_thread函数
    
    kthread_stack->function = function;//由 kernel_thread 所调用的函数
    kthread_stack->func_arg = func_arg;//这是eip所表示的函数指针中的第二个参数--要执行的函数的参数
    kthread_stack->ebp = 0;
    kthread_stack->ebx = 0;
    kthread_stack->esi = 0;
    kthread_stack->edi = 0;

}

void init_thread(struct task_struct* pthread, char* name, int prio) {
    //将pcb页清零初始化
    memset(pthread, 0 ,sizeof(struct task_struct));
    //给pcb中的一些值赋值
    strcpy(pthread->name,name);

    if(pthread == main_thread) {
        //如果是主线程的初始化，那么将执行状态设置为running，因为它已经在执行了
        pthread->status = TASK_RUNNING;
    } else {
        // 否则，将线程的运行状态设为准备运行
        pthread->status = TASK_READY;
    }

    //初始化pcb中的栈顶指针，在pcb的最顶端
    pthread->self_kstack = (uint32_t*)((uint32_t)pthread + PG_SIZE);

    // 初始化线程调度相关的参数
    pthread->priority = prio;
    //注意优先级越高，ticks越高，也就是说它运行的时间会越长，调度器只是从就绪队列中取出下一个线程来执行
    pthread->ticks = prio;
    pthread->elapsed_ticks = 0; //累计时间初始化为0
    pthread->pgdir = NULL; //线程没有自己的虚拟地址空间

    //自定义魔数
    pthread->stack_magic = 0x19870916; 
}
//创建优先级为prio的线程，名字为name
// 线程所执行的函数类型是 thread_func func
struct task_struct* thread_start(char* name, int prio, thread_func function, void* func_arg){
    // 首先申请内核空间的一页内存存放线程的PCB
    // 用户线程的PCB也在内核中，这就是所谓的内核线程实现用户线程
    struct task_struct* thread = get_kernel_pages(1);

    init_thread(thread,name,prio);
    thread_create(thread, function, func_arg);

    // 在线程创建之前，就绪队列中不应该有这个线程的general_tag
    ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
    list_append(&thread_ready_list, &thread->general_tag);

    // 在线程创建之前，一般队列中不应该有这个线程的all-tag-list
    ASSERT(!elem_find(&thread_all_list , &thread->all_list_tag));
    list_append(&thread_all_list, &thread->all_list_tag);


    // //关键 ： movl %0, %%esp 会将thread->self_kstack赋值给esp
    // // ret指令将栈顶即esp所指向地址的数据作为返回地址送上处理器的EIP寄存器，然后处理器就开始执行kernel_thread函数，然后会找到kernel_thread函数的两个参数
    // // 发现第一个参数是个函数，然后就正常执行call
    // asm volatile ("movl %0, %%esp; pop %%ebp; pop %%ebx; pop %%edi; pop %%esi;ret" :: "g" (thread->self_kstack):"memory");
    return thread;
}

// 将main函数的PCB放入全部队列中
static void make_main_thread(void) {
    main_thread = running_thread();//main_thread是一个定义在本文件开头的全局变量
    init_thread(main_thread,"main",31);

    ASSERT(!elem_find(&thread_all_list, &main_thread->all_list_tag));
    list_append(&thread_all_list, &main_thread->all_list_tag);
}

// 实现线程调度schedule
void schedule(void) {
    //在进行schedule时确保已经关中断了，保证调度程序不被打断
    ASSERT(intr_get_status() == INTR_OFF);

    struct task_struct* cur = running_thread();//获取当前线程的PCB

    if(cur->status == TASK_RUNNING) {
        //本线程的状态是running的，说明它是因为时间片用完了才被调用shedule的，
        // 将它加入就绪队列中，并将ticks重置为 当前线程的priority

        //此线程之前是RUnning的，所以不会在就绪队列中
        ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
        // 将本线程的一般标签加入就绪队列中，以便下次调用
        list_append(&thread_ready_list, &cur->general_tag);
        cur->ticks = cur->priority;
        cur->status = TASK_READY;
    }else {
        // 当前线程不是因为时间片用完才被调度的，那么肯定是由于某种原因被阻塞了（比如对0值信号量进行P操作就会让线程阻塞）
    }

    ASSERT(!list_empty(&thread_ready_list));
    thread_tag =  NULL; // 清空全局变量thread_tag 的值
    thread_tag = list_pop(&thread_ready_list);// 弹出队列中的第一个线程的thread_tag的指针，准备将其调度上CPU

    //将thread_tag指针转换成PCB的虚拟地址
    struct task_struct* next = elem2entry(struct task_struct, general_tag, thread_tag);//此宏在list.h中定义
    next->status = TASK_RUNNING;
    // 进行线程切换
    switch_to(cur, next);

}

void thread_init(void) {
    put_str("thread_init start\n");
    //初始化两个链表
    list_init(&thread_ready_list);
    list_init(&thread_all_list);
    // 将当前 main 函数创建为线程
    make_main_thread();
    put_str("thread_init donw\n");
}
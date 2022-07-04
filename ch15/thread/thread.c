#include "thread.h"
#include "debug.h"
#include "stdint.h"
#include "string.h"
#include "global.h"
#include "memory.h"
#include "print.h"
#include "interrupt.h"
#include "process.h"
#include "sync.h"

#define PG_SIZE 4096

struct task_struct* main_thread; // 主线程PCB
struct task_struct* idle_thread;    // idle线程
struct list thread_ready_list; // 就绪队列
struct list thread_all_list; // 所有任务队列
static struct list_elem* thread_tag;
struct lock pid_lock;//分配pid的锁

// 下面这个函数是用汇编写的，现在这里声明extern，以后链接时再将汇编的函数写到可执行文件中
extern void switch_to(struct task_struct* cur, struct task_struct* next);
//在main.c中的init函数，用来启动init进程
extern void init(void);
/* 系统空闲时运行的线程 */
static void idle(void* arg UNUSED) {
   while(1) {
      thread_block(TASK_BLOCKED);     
      //执行hlt时必须要保证目前处在开中断的情况下
      //hlt指令的功能让处理器停止指令执行
        // hlt执行后，cpu内部不会产生内部异常，唯一能够唤醒cpu的就是外部中断
      asm volatile ("sti; hlt" : : : "memory");
   }
}

// 获取当前线程的PCB指针
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

// 分配 pid
static pid_t allocate_pid(void) {
    static pid_t next_pid = 0;
    lock_acquire(&pid_lock);
    next_pid++;
    lock_release(&pid_lock);
    return next_pid;
}
/* fork进程时为其分配pid,因为allocate_pid已经是静态的,别的文件无法调用.
不想改变函数定义了,故定义fork_pid函数来封装一下。*/
pid_t fork_pid(void) {
   return allocate_pid();
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
    //分配pid  12章新增
    pthread->pid = allocate_pid();
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

    //14章新增，文件描述符初始化工作，处理三个标准文件描述符，其他描述符都初始化为-1
   /* 预留标准输入输出 */
   pthread->fd_table[0] = 0;
   pthread->fd_table[1] = 1;
   pthread->fd_table[2] = 2;
   /* 其余的全置为-1 */
   uint8_t fd_idx = 3;
   while (fd_idx < MAX_FILES_OPEN_PER_PROC) {
      pthread->fd_table[fd_idx] = -1;
      fd_idx++;
   }
    pthread->cwd_inode_nr = 0;//初始化：以根目录为工作路径
    pthread->parent_pid = -1;        // 默认值，-1表示没有父进程
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
        //不重置该线程的ticks,也不要把当前线程放在可执行队列中
    }

    /* 如果就绪队列中没有可运行的任务,就唤醒idle */
   if (list_empty(&thread_ready_list)) {
      thread_unblock(idle_thread);
   }

    ASSERT(!list_empty(&thread_ready_list));
    thread_tag =  NULL; // 清空全局变量thread_tag 的值
    thread_tag = list_pop(&thread_ready_list);// 弹出队列中的第一个线程的thread_tag的指针，准备将其调度上CPU

    //将thread_tag指针转换成PCB的虚拟地址
    struct task_struct* next = elem2entry(struct task_struct, general_tag, thread_tag);//此宏在list.h中定义
    next->status = TASK_RUNNING;

    // 激活进程页表等,如果当前线程是进程的话，将3特权级占保存到tss中，并切换页目录表的物理地址
    // 如果是线程的话,只需要切换页目录表的物理地址即可
    process_activate(next);

    // 进行线程切换
    switch_to(cur, next);

}

/* 主动让出cpu,换其它线程运行 */
void thread_yield(void) {
   struct task_struct* cur = running_thread();   
   enum intr_status old_status = intr_disable();
   ASSERT(!elem_find(&thread_ready_list, &cur->general_tag));
   list_append(&thread_ready_list, &cur->general_tag);
   cur->status = TASK_READY;
   schedule();
   intr_set_status(old_status);
}
void thread_init(void) {
    put_str("thread_init start\n");
    //初始化两个链表
    list_init(&thread_ready_list);
    list_init(&thread_all_list);
    lock_init(&pid_lock);//初始换分配pid的锁

     /* 先创建第一个用户进程:init */
    process_execute(init, "init");         // 放在第一个初始化,这是第一个用户进程,init进程的pid为1
    
    // 将当前 main 函数创建为线程
    make_main_thread();
    /* 创建idle线程 */
    //优先级为10
    idle_thread = thread_start("idle", 10, idle, NULL);
    put_str("thread_init donw\n");
}

// 当前线程将自己阻塞，
void thread_block(enum task_status stat) {
    ASSERT(stat == TASK_BLOCKED || TASK_WAITING || TAsK_HANGING);

    enum intr_status old_status = intr_disable();//关中断，并记录旧的中断状态

    struct task_struct* cur_thread = running_thread();//获取当前线程的taskstruct的指针
    cur_thread->status = stat;//将当前线程的状态改为阻塞状态，即TASK_BLOCKED || TASK_WAITING || TAsK_HANGING 中的一种
    schedule();//调用schedule将当前线程换下处理器,应为当前线程的状态不为RUNNNING所以调度程序知道，本线程不是因为时间片用光而被调度，而是先陷入了阻塞，所以不会将本线程的genenral_tag加入到等待队列中
    // 待当前线程接触阻塞后才继续运行 设置中断状态，
    // 注意不要自以为是地开中断，因为线程先前的中断状态是很难预测的，只能将线程一开始的中断状态记录下来，然后再用旧状态设置
    intr_set_status(old_status);
}

// 将线程pthread解除阻塞
void thread_unblock(struct task_struct* pthread) {
    enum intr_status old_status = intr_disable();// 关中断，并记录旧的中断状态

    ASSERT((pthread->status == TASK_BLOCKED) || (pthread->status == TASK_WAITING) || (pthread->status == TAsK_HANGING));

    if(pthread->status != TASK_RUNNING) {
        ASSERT(!elem_find(&thread_ready_list, &pthread->general_tag));
        if(elem_find(&thread_ready_list, &pthread->general_tag)) {
            PANIC("thread_unblock: blocked thread in ready_list\n");
        }
        //将pthread放在等待队列中，而且放在了最前端使得这个线程能够尽快被调用
        list_push(&thread_ready_list, &pthread->general_tag);

        pthread->status = TASK_RUNNING;
    }

    intr_set_status(old_status);
}


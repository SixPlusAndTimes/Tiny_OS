#ifndef __THREAD_THREAD_H
#define __THREAD_THREAD_H
#include "stdint.h"
#include "list.h"
#include "bitmap.h"
#include "memory.h"
//使用typedef定义函数类型，注意typedef定义函数指针与定义函数类型的区别！！
//https://blog.csdn.net/xiaorenwuzyh/article/details/48997767
typedef void thread_func(void*);
typedef int16_t pid_t;
//定义进程或者线程的状态
enum task_status {
    TASK_RUNNING,
    TASK_READY,
    TASK_BLOCKED,
    TASK_WAITING,
    TAsK_HANGING,
    TASK_DIED
};

// 中断栈intr_stack
//用于中断发生时保护程序的上下文
// 中断栈在内核栈中的位置是固定的，在PCB页的最顶端
struct intr_stack {
    uint32_t vec_no; // kernel.S 宏 VECTOR 中 %1 压入的中断号
    uint32_t edi;
    uint32_t esi;
    uint32_t ebp;
    uint32_t esp_dummy;	 // 虽然pushad把esp也压入,但esp是不断变化的,所以会被popad忽略
    uint32_t ebx;
    uint32_t edx;
    uint32_t ecx;
    uint32_t eax;
    uint32_t gs;
    uint32_t fs;
    uint32_t es;
    uint32_t ds;

    // 以下由 cpu 从低特权级进入高特权级时压入
    uint32_t err_code;		 // err_code会被压入在eip之后
    void (*eip) (void);
    uint32_t cs;
    uint32_t eflags;
    void* esp;
    uint32_t ss;
};

//线程自己的栈，用于存储线程中待执行的函数
// 此结构在自己的内核栈中的位置不固定
// 在switch_to时保存上下文环境
struct thread_stack {
    uint32_t ebp;
    uint32_t ebx;
    uint32_t edi;
    uint32_t esi;

    //线程首次执行时，eip指向待调用的函数kernel_thread
    //其他情况下，eip指向switch_to的返回地址
    void (*eip) (thread_func* func, void* func_arg);

    // 下面的数据只在第一次被调度上cpu时使用

    //返回地址，仅供占用，没有实际的作用，因为 第一次（注意是第一次） kernel_thread的执行不是通过call来调用的，而是通过ret来调用的！！
    //但是为了符合C语言的函数调用规约，我们必须在栈顶放上放回地址，这个返回地址不由作用，只是充数的，为了符合规定
    // 否则在kernel_thread执行过程中会找错参数的位置而出错
    void (*unused_retaddr);
    thread_func* function; // 由 kernel_thread 所调用的函数
    void* func_arg; // 由 kernel_thread 所调用的函数所需的参数
};

//进程的pcb控制块
// 以上两个栈结构都位于内核栈中，都位于PCB的高地址处
struct task_struct {
    uint32_t* self_kstack; // 每个线程都有一个自己的内核栈，存储的是thread_stack地址
    pid_t pid;
    enum task_status status;

    char name[16];

    uint8_t priority; // 线程优先级
    uint8_t ticks; // 每一次 该线程在处理器上被分配的时间片
    uint8_t elapsed_ticks; //此任务自第一次被执行以来，一共执行了多少个时间片
    
    struct list_elem general_tag;// 位于一般线程队列中的节点的标签
    struct list_elem all_list_tag; // 位于 thread_all_list中的节点； 每个线程PCB的此属性都会加入到thread_all_list链表中

    uint32_t* pgdir;// 进程自己的页表的虚拟地址
    //进程有自己的页表，但是线程没有，线程的这个属性设置为NULL，这就是线程和进程的最大区别
    struct virtual_addr userprog_vaddr; //用户进程的虚拟地址
    struct mem_block_desc u_block_desc[DESC_CNT];  //用户进程的内存块描述符

    uint32_t stack_magic; //魔数，用于检测栈的溢出;想想struct结构各个成员在物理地址上的分布就可以明白了

};

extern struct list thread_ready_list;
extern struct list thread_all_list;

void thread_create(struct task_struct* pthread, thread_func function, void* func_arg);
void init_thread(struct task_struct* pthread, char* name, int prio);
struct task_struct* thread_start(char* name, int prio, thread_func function, void* func_arg);

struct task_struct* running_thread(void);
void schedule(void);
void thread_init(void);
void thread_block(enum task_status stat);
void thread_unblock(struct task_struct* pthread);
void thread_yield(void);
#endif
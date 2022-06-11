#include "process.h"
#include "global.h"
#include "debug.h"
#include "memory.h"
#include "thread.h"    
#include "list.h"    
#include "tss.h"    
#include "interrupt.h"
#include "string.h"
#include "console.h"

//这个声明在这里是干什么？？
extern void intr_exit(void);

// 手动构建用户进程的上下文信息
void start_process(void* filename_) {
    void* function = filename_;
    struct task_struct* cur = running_thread();
    cur->self_kstack += sizeof(struct thread_stack);//将self_kstack指向中断栈的栈顶
    struct intr_stack* proc_stack = (struct intr_stack*)cur->self_kstack;

    proc_stack->edi = proc_stack->esi = proc_stack->ebp = proc_stack->esp_dummy = 0;
    proc_stack->ebx = proc_stack->edx = proc_stack->ecx = proc_stack->eax = 0;
    proc_stack->gs = 0; // 用户态不允许访问现存, gs直接初始为 0
    proc_stack->ds = proc_stack->es = proc_stack->fs = SELECTOR_U_DATA; //为用户态的段寄存器赋值
    proc_stack->eip = function; // 待执行的用户程序地址
    proc_stack->cs = SELECTOR_U_CODE;
    proc_stack->eflags = (EFLAGS_IOPL_0 | EFLAGS_MBS | EFLAGS_IF_1); //EFLAGS寄存器的IF位应该置为1，因为当用户进程在运行的时候应该是开中断的状态


    // USER_STACK3_VADDR = (0xc0000000 - 0x1000)
    // 因为在4GB的虚拟地址空间中，0xc0000000是用户空间的最高地址，因此0xc0000000 - 0x1000是用户空间中顶层的一页的最低地址
    // 用USER_STACK3_VADDR分配到实际的物理地址后，再是得esp指向USER_STACK3_VADDR + PG+SIZE， 这样esp就是名副其实的3特权级下的栈顶指针了
    proc_stack->esp = (void*)((uint32_t)get_a_page(PF_USER, USER_STACK3_VADDR) + PG_SIZE);//为用户进程分配3特权级的栈
    proc_stack->ss = SELECTOR_U_DATA;
    asm volatile("movl %0, %%esp; jmp intr_exit" : : "g" (proc_stack) : "memory");
}

// 激活页表
void page_dir_activate(struct task_struct* p_thread) {
    // 默认为内核所用的 页目录项表的物理地址
    uint32_t pagedir_phy_addr = 0x100000;
    if(p_thread->pgdir != NULL) {
        // 如果是进程而不是内核线程，就将页目录表的物理地址重置
        pagedir_phy_addr = addr_v2p((uint32_t)p_thread->pgdir);
    }
    //内联汇编重置cr3寄存器
    asm volatile("movl %0, %%cr3" : : "r" (pagedir_phy_addr) : "memory");
}

// 激活线程或进程的页表, 更新 tss 中的 esp0 为进程的特权级 0 的栈
void process_activate(struct task_struct* p_thread) {
    ASSERT(p_thread != NULL);
    page_dir_activate(p_thread);
    // 内核线程特权级本身就是 0, 处理器进入中断时并不会从
    // tss 中获取 0 特权级栈地址, 故不需要更新 esp0

    if(p_thread->pgdir) {
        //如果是用户进程，那就需要设置它的高特权级栈
        // 更新该进程的 esp0, 用于此进程被中断时保留上下文
        update_tss_esp(p_thread);
    }
}

// 创建用户进程的页目录，将内核使用的256个PDE复制到用户进程的头部
uint32_t* create_page_dir(void) {
    // 用户进程的页表不能让用户直接访问到，所以再内核空间申请页目录表
    uint32_t* page_dir_vaddr = get_kernel_pages(1);
    if (page_dir_vaddr == NULL) {
        // 分配失败
        console_put_str("create_page_dir: get_kernel_pages failed!");
        return NULL;
    }

    // 复制内核页目录项的前256到新页表的前256个项
    // 0xfffff000 + 0x300 * 4 是内核页目录的第 768 项
    // 256 * 4 = 1024字节
    // 就是将从内核目录的第768项的地址开始，复制1024个字节到新创建的页目录表的的768项， 从而完成了内核pde的复制
    memcpy( (uint32_t*) ((uint32_t)page_dir_vaddr + 0x300*4 ), (uint32_t*)(0xfffff000 + 0x300*4), 1024 );

    //修改最后一个页目录项，使它指向用户目录表的起始物理地址
    uint32_t new_page_dir_phy_addr = addr_v2p((uint32_t)page_dir_vaddr) | PG_US_U | PG_RW_W | PG_P_1;;
    page_dir_vaddr[1023] = new_page_dir_phy_addr;

    return page_dir_vaddr;
}

// 创建用户进程虚拟地址位图
void create_user_vaddr_bitmap(struct task_struct* user_prog) {
    user_prog->userprog_vaddr.vaddr_start = USER_VADDR_START;
    // 管理某个进程的虚拟的位图一共占用bitmap_pg_cnt个内核页面
    uint32_t bitmap_pg_cnt = DIV_ROUND_UP((0xc0000000 - USER_VADDR_START) / PG_SIZE / 8, PG_SIZE);
    // 使用get_kernel_pages(bitmap_pg_cnt) 申请对应的内核页数
    user_prog->userprog_vaddr.vaddr_bitmap.bits = get_kernel_pages(bitmap_pg_cnt);
    user_prog->userprog_vaddr.vaddr_bitmap.btmp_bytes_len = (0xc0000000 - USER_VADDR_START) / PG_SIZE / 8;
    bitmap_init(&user_prog->userprog_vaddr.vaddr_bitmap);
}

// 创建用户进程
void process_execute(void* filename, char* name) {
    // 进程的pcb一样也在内核物理池中
    struct task_struct* thread = get_kernel_pages(1);
    // 默认优先级为31
    init_thread(thread, name, default_prio);
    create_user_vaddr_bitmap(thread);

    // start_process函数作为调用函数而不是kernel_thread_start,filename是用户进程地址
    thread_create(thread,start_process,filename);
    // 为用户进程创建页表
    thread->pgdir = create_page_dir();

    enum intr_status old_status = intr_disable();
    ASSERT(!elem_find(&thread_ready_list, &thread->general_tag));
    list_append(&thread_ready_list, &thread->general_tag);

    ASSERT(!elem_find(&thread_all_list, &thread->all_list_tag));
    list_append(&thread_all_list, &thread->all_list_tag);

    intr_set_status(old_status);

}
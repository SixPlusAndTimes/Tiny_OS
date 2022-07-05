#include "interrupt.h"
#include "stdint.h"
#include "global.h"
#include "io.h"

#define PIC_M_CTRL 0x20 // 可编程中断控制器是 8259A, 主片的控制端口是 0x20
#define PIC_M_DATA 0x21 // 主片的数据端口是 0x21
#define PIC_S_CTRL 0xa0 // 从片的控制端口是 0xa0
#define PIC_S_DATA 0xa1 // 从片的数据端口是 0xa1

#define IDT_DESC_CNT 0x81 // 目前总共支持的中断数

#define EFLAGS_IF 0x00000200 // eflags寄存器中的if位 为1
#define GET_EFLAGS(EFLAG_VAR) asm volatile("pushfl; popl %0" : "=g" (EFLAG_VAR))

extern uint32_t syscall_handler(void);

// 中断门描述符结构体
struct gate_desc {
    uint16_t func_offset_low_word;
    uint16_t selector;
    uint8_t  dcount;
    uint8_t  attribute;
    uint16_t func_offset_high_word;
};

// 静态函数声明, 非必须
static void make_idt_desc(struct gate_desc* p_gdesc, uint8_t attr, intr_handler function);
static struct gate_desc idt[IDT_DESC_CNT]; // idt 是中断描述符表

char* intr_name[IDT_DESC_CNT]; // 用于保存异常的名字
// intr_handler的定义在interrupt.h中 ： 
// typedef void* intr_handler; 即intr_handler 就是一个普通指针
// intr_handler idt_table[IDT_DESC_CNT]; ==> void* intr_entry_table[IDT_DESC_CNT]
// extern intr_handler intr_entry_table[IDT_DESC_CNT];  ==> extern void* intr_entry_table[IDT_DESC_CNT];
intr_handler idt_table[IDT_DESC_CNT]; // 定义中断处理程序数组
extern intr_handler intr_entry_table[IDT_DESC_CNT]; // 声明引用定义在 kernel.S 中的中断处理入口函数数组

// 初始化 8259A
/* 初始化可编程中断控制器8259A */
static void pic_init(void) {

   /* 初始化主片 */
   outb (PIC_M_CTRL, 0x11);   // ICW1: 边沿触发,级联8259, 需要ICW4.
   outb (PIC_M_DATA, 0x20);   // ICW2: 起始中断向量号为0x20,也就是IR[0-7] 为 0x20 ~ 0x27.
   outb (PIC_M_DATA, 0x04);   // ICW3: IR2接从片. 
   outb (PIC_M_DATA, 0x01);   // ICW4: 8086模式, 正常EOI

   /* 初始化从片 */
   outb (PIC_S_CTRL, 0x11);    // ICW1: 边沿触发,级联8259, 需要ICW4.
   outb (PIC_S_DATA, 0x28);    // ICW2: 起始中断向量号为0x28,也就是IR[8-15] 为 0x28 ~ 0x2F.
   outb (PIC_S_DATA, 0x02);    // ICW3: 设置从片连接到主片的IR2引脚
   outb (PIC_S_DATA, 0x01);    // ICW4: 8086模式, 正常EOI
   
  /* IRQ2用于级联从片,必须打开,否则无法响应从片上的中断
  主片上打开的中断有IRQ0的时钟,IRQ1的键盘和级联从片的IRQ2,其它全部关闭 */
   outb (PIC_M_DATA, 0xf8);

/* 打开从片上的IRQ14,此引脚接收硬盘控制器的中断 */
   outb (PIC_S_DATA, 0xbf);

   put_str("   pic_init done\n");
}

// 创建中断门描述符
static void make_idt_desc(struct gate_desc* p_gdesc, uint8_t attr, intr_handler function) {
    p_gdesc->func_offset_low_word = (uint32_t)function & 0x0000FFFF;
    p_gdesc->selector = SELECTOR_K_CODE;
    p_gdesc->dcount = 0;
    p_gdesc->attribute = attr;
    p_gdesc->func_offset_high_word = ((uint32_t)function & 0xFFFF0000) >> 16;
}

// 初始化中断描述符表
static void idt_desc_init(void) {
    int i, last_index = IDT_DESC_CNT - 1;
    for(i = 0; i < IDT_DESC_CNT; i++) {
        make_idt_desc(&idt[i], IDT_DESC_ATTR_DPL0 , intr_entry_table[i]);
    }

    //单独处理系统调用，注意，系统调用对应的中断门DPL为3
    // 中断处理程序为单独的syscall_handler
    make_idt_desc(&idt[last_index], IDT_DESC_ATTR_DPL3,syscall_handler);
    put_str("idt_desc_init done\n");
}

// 通用的中断处理函数，一般是处理异常的情况，其他中断比如时钟中断会有专门的处理函数处理
static void general_intr_handler(uint8_t vec_nr) {
    if(vec_nr == 0x27 || vec_nr == 0x2f) {
        // IRQ7 和 IRQ15 会产生伪中断, 无需处理
        return;
    }
    // 将光标置为屏幕左上角, 清理一块区域，方便观察错误信息
    set_cursor(0);
    int cursor_pos = 0;
    while(cursor_pos < 320) {
        put_char(' ');
        cursor_pos++;
    }
    // 将光标重新置为屏幕左上角
    set_cursor(0);
    put_str("!!!!! exception message begin !!!!!\n");
    set_cursor(88); // 从第 2 行第 8 个字符开始打印
    put_str(intr_name[vec_nr]);
    if(vec_nr == 14) { // 若为 Pagefault, 将缺失的地址打印出来并悬停
        int page_fault_vaddr = 0;
        // cr2 存放造成 page_fault 的地址
        asm("movl %%cr2, %0" : "=r" (page_fault_vaddr));
        put_str("\npage fault addr is "); put_int(page_fault_vaddr);
    }

    put_str("\n!!!!! exception message end !!!!!\n");

    // 已经进入中断处理程序就表示已经处在关中断情况下
    // 不会出现线程调度的情况, 故下面的死循环不会再被中断
    while(1);
}

// 完成一般中断处理函数注册及异常名称注册
static void exception_init(void) {
    int i;
    for(i = 0; i < IDT_DESC_CNT; i++) {
        idt_table[i] = general_intr_handler;
        intr_name[i] = "unknown";
    }
    intr_name[0] = "#DE Divide Error";
    intr_name[1] = "#DB Debug Exception";
    intr_name[2] = "NMI Interrupt";
    intr_name[3] = "#BP Breakpoint Exception";
    intr_name[4] = "#OF Overflow Exception";
    intr_name[5] = "#BR BOUND Range Exceeded Exception";
    intr_name[6] = "#UD Invalid Opcode Exception";
    intr_name[7] = "#NM Device Not Available Exception";
    intr_name[8] = "#DF Double Fault Exception";
    intr_name[9] = "Coprocessor Segment Overrun";
    intr_name[10] = "#TS Invalid TSS Exception";
    intr_name[11] = "#NP Segment Not Present";
    intr_name[12] = "#SS Stack Fault Exception";
    intr_name[13] = "#GP General Protection Exception";
    intr_name[14] = "#PF Page-Fault Exception";
    // intr_name[15] 第15项是intel保留项，未使用
    intr_name[16] = "#MF x87 FPU Floating-Point Error";
    intr_name[17] = "#AC Alignment Check Exception";
    intr_name[18] = "#MC Machine-Check Exception";
    intr_name[19] = "#XF SIMD Floating-Point Exception";
}
 
 //开中断并返回开中断之前的状态
enum intr_status intr_enable() {
    enum intr_status old_status;
    if(INTR_ON == intr_get_status()) {
        old_status = INTR_ON;
        return old_status;
    } else{
        old_status = INTR_OFF;
        asm volatile("sti");//开中断
        return old_status;
    }
}
//关中断并返回开中断之前的状态
enum intr_status intr_disable() {
    enum intr_status old_status;
    if(INTR_OFF == intr_get_status()){
        old_status = INTR_OFF;
        return old_status;
    } else {
        old_status = INTR_ON;
        asm volatile("cli" : : : "memory");
        return old_status;
    }
}

//将中断设置为 status
enum intr_status intr_set_status(enum intr_status status) {
    return status & INTR_ON ? intr_enable() : intr_disable();
}

// 获取当前中断状态
enum intr_status intr_get_status() {
    uint32_t eflags = 0;
    GET_EFLAGS(eflags);
    return (EFLAGS_IF & eflags) ? INTR_ON : INTR_OFF;
}

// 在中断处理程序数组的第 vector_no 个元素中，注册中断处理函数
void register_handler(uint8_t vector_no, intr_handler function) {
    idt_table[vector_no] = function;
}

// 完成有关中断的所有初始化工作
void idt_init() {
    put_str("idt_init start\n");
    idt_desc_init(); // 初始化中断描述符表
    exception_init(); // 异常名初始化并注册通常的中断处理函数
    pic_init();      // 初始化 8259A

    // 加载 idt
    // sizeof((idt) - 1)得到短截线limit，用作低16位的段界限
    // ((uint64_t)(uint32_t)idt << 16) 将idt的自制挪到高32位
    // m 表示使用内存约束，所以lidt 的操作数是&idt_operand 而不是idt_operand的值
    uint64_t idt_operand = ((sizeof(idt) - 1) | ((uint64_t)(uint32_t)idt << 16));
    asm volatile("lidt %0" : : "m" (idt_operand));
    put_str("idt_init donw\n");
}
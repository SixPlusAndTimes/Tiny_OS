#include "timer.h"
#include "io.h"
#include "interrupt.h"
#include "debug.h"
#include "print.h"
#include "thread.h"

#define IRQ0_FREQUENCY   100
#define INPUT_FREQUENCY  1193180
#define COUNTER0_VALUE   INPUT_FREQUENCY / IRQ0_FREQUENCY
#define COUNTER0_PORT    0x40
#define COUNTER0_NO      0
#define COUNTER_MODE     2
#define READ_WRITE_LATCH 3
#define PIT_CONTROL_PORT 0x43

uint32_t ticks;//内核自启动以来的滴答数
static void intr_timer_handler(void) {
    struct task_struct* cur_thread = running_thread();

    ASSERT(cur_thread->stack_magic == 0x19870916);//检查栈是否溢出

    cur_thread->elapsed_ticks++;//记录此线程占用的cpu时间
    ticks++; //内核嘀嗒数也加一

    if(cur_thread->ticks == 0) {
        schedule();//如果该线程的滴答数为零，就调用schedule函数
    } else {
        //如果ticks没有用完，就减0
        cur_thread->ticks--;
    }
}

static void frequency_set(uint8_t counter_port,
                          uint8_t counter_no,
                          uint8_t rwl,
                          uint8_t counter_mode,
                          uint16_t counter_value) {
    // 往控制字寄存器端口0x43中写入控制字
   outb(PIT_CONTROL_PORT, (uint8_t)(counter_no << 6 | rwl << 4 | counter_mode << 1));
    // 先写入counter_value的低8位
   outb(counter_port, (uint8_t)counter_value);
    // 再写入counter_value的高8位
   outb(counter_port, (uint8_t)counter_value >> 8);
}

// 初始化 PIT8253
void timer_init() {
    put_str("timer_init start\n");
    // 设置 8253 的定时周期
    frequency_set(COUNTER0_PORT, COUNTER0_NO, READ_WRITE_LATCH, COUNTER_MODE, COUNTER0_VALUE);
    register_handler(0x20, intr_timer_handler);
    put_str("timer_init donw\n");
}
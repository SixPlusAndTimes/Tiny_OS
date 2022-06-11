#include "init.h"
#include "print.h"
#include "interrupt.h"
#include "timer.h"
#include "memory.h"
#include "console.h"

// 初始化所有模块
void init_all() {
   put_str("init_all\n");
   idt_init();    // 初始化中断
   mem_init();	  // 初始化内存管理系统
   thread_init(); // 初始化线程相关结构
   timer_init();  // 初始化PIT
   console_init();//控制台初始化
   keyboard_init(); // 键盘初始化
}
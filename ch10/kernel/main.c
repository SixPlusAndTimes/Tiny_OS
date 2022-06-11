#include "print.h"
#include "init.h"
#include "debug.h"
#include "memory.h"
#include "thread.h"
#include "console.h"
#include "ioqueue.h"
#include "keyboard.h"
#include "interrupt.h"



void k_thread_a(void*);
void k_thread_b(void*);

int main(void) {
   put_str("I am kernel\n");
   init_all();

  thread_start("k_thread_a", 31, k_thread_a, "argA ");
  thread_start("k_thread_b", 31, k_thread_b, "argB ");

   intr_enable();
   while(1); //{
      //console_put_str("Main ");
  // };
   return 0;
}

/* 在线程中运行的函数 */
void k_thread_a(void* arg) {     
    while(1) {
            console_put_str(arg);
    }
}

/* 在线程中运行的函数 */
void k_thread_b(void* arg) {     
    while(1) {
            console_put_str(arg);

    }
}

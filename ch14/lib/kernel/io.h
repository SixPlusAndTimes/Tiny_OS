#ifndef __LIB_IO_H
#define __LIB_IO_H
#include "stdint.h"
// 注意这个头文件 包含了函数的实现
// 这样的坏处是，每个include此文件的程序都有一份拷贝，所以有可能在一个工程中包含此头文件的多个拷贝，空间消耗大
//  好处是：注意到这里的函数都是static，表示函数只能在本文件中使用，而且inline修饰，表示编译器可以对函数进行内联优化
// 内联的好处是：减少函数调用的开销， 这样对硬件端口的操作就可以快一些了

// 向端口 port 写入一个字节
static inline void outb(uint16_t port, uint8_t data) {
    asm volatile ("out %b0, %w1" : : "a" (data), "Nd" (port));
}

// 将 addr 处起始的 word_cnt 个字写入端口 port
static inline void outsw(uint16_t port, const void* addr, uint32_t word_cnt) {
    asm volatile ("cld; rep outsw" : "+S" (addr), "+c" (word_cnt) : "d" (port));
}

// 将从端口 port 读入的 word_cnt 个字写入 addr
// cld： 清楚方向为DF，所以EDI的值将自动增加
// +D表示用寄存器约束D将变量addr的值约束到EDI中，其中 + 表示既做输入也做输出
// +c表示将变量word_cnt约束到cx中，同样即作输出也做输入
static inline void insw(uint16_t port, void* addr, uint32_t word_cnt) {
    asm volatile ("cld; rep insw" : "+D" (addr), "+c" (word_cnt) : "d" (port) : "memory");
}

// 将从端口 port 读入一个字节返回
static inline uint8_t inb(uint16_t port) {
    uint8_t data;
    asm volatile ("inb %w1, %b0" : "=a" (data) : "Nd" (port));
   return data;
}
#endif
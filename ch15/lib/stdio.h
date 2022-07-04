#ifndef __LIB_STDIO_H
#define __LIB_STDIO_H
#include "stdint.h"
typedef char* va_list;
//pring函数是对vprintf和sprintf两个函数地封装
uint32_t printf(const char* str, ...);
//将format中%的部分用ap中的参数替换，并将结果拷贝拷贝到str中
uint32_t vsprintf(char* str, const char* format, va_list ap);
uint32_t sprintf(char* buf, const char* format, ...);
#endif
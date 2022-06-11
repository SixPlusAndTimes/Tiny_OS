#ifndef __KERNEL_DEBUG_H
#define __KERNEL_DEBUG_H
void panic_spin(char* fileName, int line, const char* func, const char* condition);
/*
 * __VA_ARGS__ 是预处理器所支持的专用标识符。
 * 代表所有与省略号相对应的参数. 
 * "..."表示定义的宏其参数可变.*/
/*
*　__FILE__、__LINE__、__func__预编译的宏。
* __FILE__，其表明当前行语句所在的文件名称。
* __LINE__,其表明当前行语句所在的文件中的行。
* __func__，其表明当前行语句所在的父函数的名称。
*/
#define PANIC(...) panic_spin(__FILE__, __LINE__, __func__, __VA_ARGS__)

#ifdef NDEGUG //如果不调试时，应该让ASSERT宏失效，这样编译出的文件会变小一点
    #define ASSERT(CONDITION) ((void)0)
#else
// 注意： 多行宏用反斜杠隔开
    #define ASSERT(CONDITION) \
        if (CONDITION){} else { \
            /* 符号#让编译器将宏的参数转化为字符串字面量 */\
    PANIC(#CONDITION);  \
        }
#endif

#endif

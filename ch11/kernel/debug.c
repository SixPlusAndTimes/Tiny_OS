#include "debug.h"
#include "print.h"
#include "interrupt.h"

void panic_spin(char* fileName, int line, const char* func, const char* condition) {
    intr_disable();//关中断
    put_str("\n\n!!!!erroe!!!!\n");
    put_str("filename:");put_str(fileName);put_str("\n");
    put_str("line:");put_int(line);put_str("\n");
    put_str("condition:");put_str((char)* condition);put_str("\n");
}
#include "string.h"
#include "global.h"
#include "debug.h"
#include "assert.h"

//将dst_起始的size个字节设置为value
void memset(void* dst_, uint8_t value, uint32_t size) {
    assert(dst_ != NULL);
    uint8_t* dst = (uint8_t*)dst_; //告诉编译器将dst这里开始的内存，以无符号8位整数进行解析
    while(size-->0) *dst++ = value;//自增运算符比解引用的优先级高。
}

//将src起始的size个字节复制到dst
void memcpy(void* dst_, const void * src_, uint32_t size){
    assert(dst_ != NULL && src_ != NULL);
    uint8_t* dst = dst_;
    const uint8_t* src = src_;
    while(size-- > 0) {
        *dst++ = *src++;
    }
}

//比较地址a和地址b开头的size个字节，若相等则返回0， a大于b则返回1， 否则返回-1
int memcmp(const void* a_, const void* b_ ,uint32_t size) {
    const char* a = a_;
    const char* b = b_;
    assert(a != NULL || b != NULL);
    while(size-- > 0) {
        if(*a != *b){
            return *a > *b? 1 : -1;
        }
        a++;
        b++;
    }
    return 0;
}

//将字符串从src复制到dst
char* strcpy(char* dst_, const char* src_) {
    assert(dst_ != NULL && src_ != NULL);
    char* r = dst_; //返回值
    while((*dst_++ = *src_++)); //字符串结束字符位以整形表示为0， 所以当 *dst_等于0时，表示字符串已经遍历到了末尾，退出循环
    return r;
}

//返回字符串长度
uint32_t strlen(const char* str){
    assert(str != NULL);
    const char* p = str;
    while(*p++);
    return (p - str - 1);
}

/* 比较两个字符串,若a_中的字符大于b_中的字符返回1,相等时返回0,否则返回-1. */
int8_t strcmp(const char* a, const char* b) {
    assert(a != NULL && b != NULL);
    while(*a != 0 && *a == *b) {
        a++;
        b++;
    }
    return *a < *b ? -1 : *a > *b;
}

//从左到右查找字符串str首次出现字符ch的地址
char* strchr(const char* str, const uint8_t ch){
    assert(str != NULL);
    while(*str !=0 ) {
        if(*str == ch) {
            return (char*)str;
        }
        str++;
    }
    return NULL;
}

//从后往前查找字符串str中首次出现字符ch的地址
char* strrchr(const char* str, const uint8_t ch) {
    assert (str != NULL);
    const char* last_char = NULL;
    while (*str != 0)
    {
        if(*str == ch){
            last_char = str;
        }
        str++;
    }
    return (char*) last_char;
}

//将字符串src拼接到dst之后，返回拼接的串地址
char* strcat(char* dst_, const char* src_) {
    assert(dst_ != NULL && src_ != NULL);
    char* str = dst_;
    while(*str++);//找到目标字符串的最后一个字符，及空字符
    --str;//跳过空字符
    while ((*str++ = *src_++));
    return dst_;
}

//在字符串str中查找字符ch出现的次数
uint32_t strchrs(const char* str , uint8_t ch) {
    assert(str != NULL);
    uint32_t ch_cnt = 0;
    const char* p = str;
    while(*p == ch) {
        if(*p == ch) {
            ch_cnt++;
        }
         p++;
    }
    return ch_cnt;
}


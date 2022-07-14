#ifndef __KERNEL_MEMORY_H
#define __KERNEL_MEMORY_H 
#include "stdint.h"
#include "bitmap.h"
#include "list.h"

// 内存池标记, 用于判断是哪个内存池，用户内存池还是内核内存池
enum pool_flags {
    PF_KERNEL = 1,
    PF_USER = 2
};

#define PG_P_1 1 //页表项或目录项存在属性位
#define PG_P_0 0 
#define PG_RW_R 0 //RW属性值， 读/执行
#define PG_RW_W 2 // RW属性值,读/写/执行
#define PG_US_S 0    //US属性位，系统级
#define PG_US_U 4   //US属性位置，用户级

//虚拟地址池，用于虚拟地址管理
struct virtual_addr {
    struct bitmap vaddr_bitmap; //虚拟地址用到的位图结构
    uint32_t vaddr_start; //虚拟地址起始地址
};

// 内存块
struct mem_block {
   struct list_elem free_elem;
};


struct mem_block_desc {
   uint32_t block_size;		 // 内存块大小
   uint32_t blocks_per_arena;	 // 这个类型的arena能够存放几个mem_block
   struct list free_list;	 // 目前可用的mem_block链表，记录所有同类型的arena的空闲内存块
};

#define DESC_CNT 7  //内存块描述符的个数，有7中规格 16字节 32字节 64字节 128字节 256字节 512字节 1024字节;当要分配的内存块大于1024时，直接分配一个页框而不在arena中分配

extern struct pool kernel_pool, user_pool;
void mem_init(void);
void* get_kernel_pages(uint32_t pg_cnt);
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt);
void malloc_init(void);
uint32_t* pte_ptr(uint32_t vaddr);
uint32_t* pde_ptr(uint32_t vaddr);
uint32_t addr_v2p(uint32_t vaddr);
void* get_a_page(enum pool_flags pf, uint32_t vaddr);
void* get_user_pages(uint32_t pg_cnt);
void block_desc_init(struct mem_block_desc* desc_array);
void* sys_malloc(uint32_t size); //malloc系统调用子处理函数
void mfree_page(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt);
void pfree(uint32_t pg_phy_addr);
void sys_free(void* ptr);
void* get_a_page_without_opvaddrbitmap(enum pool_flags pf, uint32_t vaddr);
void free_a_phy_page(uint32_t pg_phy_addr);
#endif
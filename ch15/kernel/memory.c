#include "memory.h"
#include "bitmap.h"
#include "stdint.h"
#include "global.h"
#include "debug.h"
#include "print.h"
#include "string.h"
#include "sync.h"
#include "interrupt.h"

#define PG_SIZE 4096 //页面的大小 = 4096字节 = 4KB

//定义位图地址
#define MEM_BITMAP_BASE 0xc009a000 
//此地址映射到物理内存低端1MB的9a000处，
// 每个进程都有一个PCB，PCB要占用4KB也就是一页大小的内存空间
// 而且PCB所占的内存必须时自然页，也就是页的起始地址为0xxxxxx000---12位为0；
// 程序都有个主线程，内核也是一样有个main线程，它一直存在，它也需要一个PCB
// LOADER.S中，在进入内核前，通过mov esp, 0x9f000作为内核栈顶
// 而栈在PCB的最顶端， 所以main线程的PCB其实地址是 栈顶指针地址减去一页的大小 = 0x9e00；
// 也就是说PCB起始地址在 0x9e00
// 我们需要在卖弄线程的PCB起始地址更低的物理地址上给位图分配地址，下面来看看我们需要分配多大的位图
// 32MB的内存 需要1024个字节（32MB/4KB = 8KB 个位； 8KB个字节 / 8 = 1024 个字节)的位图，也就是仅仅占用四分之一个页
// 也就是说，一页可以管理128MB内存，书上使用一个四个页存储位图，也就是说一共可以管理512MB的内存
// 那么位图的起始位置为PCB页的起始地址减去4个页的大小，就等于9a000

// 0xc0100000是内核从虚拟地址3G起，如果内核要申请额外的堆内存，这是起始地址
#define K_HEAP_START 0xc0100000

#define PDE_IDX(addr) ((addr & 0xffc00000) >> 22) // 取得高10位，获取页目录表下标
#define PTE_IDX(addr) ((addr & 0x003ff000) >> 12) // 取得中间10位，注意先屏蔽高10位，获取页表下标

// 内存池结构，生成两个实例用于管理内核池和用户内存池
// 与虚拟内存管理结构相比，物理池管理结构多了一个pool_size，而虚拟内存管理结构就没有这个属性，因为虚拟地址相对来说是不受限制的
struct pool {
    struct bitmap pool_bitmap; //本内存池位图结构，管理物理内存
    uint32_t phy_addr_start; //本内存池所管理物理内存的起始地址
    uint32_t pool_size; // 本内存池字节容量

    struct lock lock;    
};

//内核与用户物理池管理结构，都是全局变量，在mem_pool_init中被初始化
struct pool kernel_pool, user_pool;

struct virtual_addr kernel_vaddr;

// arena结构的元信息
struct arena {
   struct mem_block_desc* desc;	 // 此arena关联的mem_block_desc
    // large 为 true 时, cnt 表示的是页框数
    // 否则 cnt 表示空闲的 mem_block 数量
   uint32_t cnt;
   bool large;		   
};

struct mem_block_desc k_block_descs[DESC_CNT];//内核内存块描述驻足

static void* vaddr_get(enum pool_flags pf, uint32_t pg_cnt) {
    int vaddr_start = 0, bit_idx_start = -1;
    uint32_t cnt = 0;
    if(pf == PF_KERNEL) { // 内核内存池
        bit_idx_start = bitmap_scan(&kernel_vaddr.vaddr_bitmap, pg_cnt);
        if(bit_idx_start == -1) {
            return NULL;
        }
        while(cnt < pg_cnt) {
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 1);
        }
        vaddr_start = kernel_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
    } else { // 用户内存池
        //首先取出PCB
        struct task_struct* cur = running_thread();
        bit_idx_start = bitmap_scan(&cur->userprog_vaddr.vaddr_bitmap, pg_cnt);
        if(bit_idx_start == -1) {
            return NULL;
        }
        while(cnt < pg_cnt) {
            bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx_start+cnt++, 1);
        }
        vaddr_start = cur->userprog_vaddr.vaddr_start + bit_idx_start * PG_SIZE;
        ASSERT((uint32_t)vaddr_start < (0xc0000000 - PG_SIZE));
    }
    return (void*)vaddr_start;
}

// 得到虚拟地址vaddr对应的pte指针
// 注意得到的pte指针也应该是虚拟地址，但是它现在已经对应一份物理内存了
// 分成三部分拼凑这个pte指针
// 首先pte虚拟地址的高10位应该指向第1023个目录项，因为最后一个目录项中的地址存放的是页目录的地址0x100000，这样处理器访问的时候就会认为它在访问页表，单实际上还是在访问页目录表
    // 1023 = 0x3ff, 右移10位后得0xffc00000，这就是pte虚拟指针的高10位
// 然后处理器认为虚拟地址的中间10位是页表项索引，但其实它现在要处理的是页目录项索引，这样才能找到页表的物理地址。如何得到页目录项索引?
    //将虚拟地址的pde索引抽出来就行了， vaddr & 0xffc00000 >> 10
// 最后处理器认为它现在已经在普通页表中了，但实际上它还在页表中，为了得到页表项的物理地址，首先得知道vaddr的pte索引，然后乘以四，作为pte指针虚拟地址的最后12位
uint32_t* pte_ptr(uint32_t vaddr) {
    uint32_t* pte = (uint32_t*) (0xffc00000 + ((vaddr & 0xffc00000) >> 10) + PTE_IDX(vaddr) *4 );
    return pte;
}

//得到虚拟地址vaddr的pde指针
//pte_ptr和pde_ptr这两个函数中的参数vaddr，可以是已分配、在页表中，也可以是尚未分配，目前页表中不存在的虚拟地址
uint32_t* pde_ptr(uint32_t vaddr) {
    uint32_t* pde = (uint32_t*)((0xfffff000) + PDE_IDX(vaddr) * 4);
    return pde;
}

//在mpool指向的物理内存池中分配1个物理页，成功则返回物理页框的物理地址，失败则返回NULL
static void* palloc(struct pool* m_pool) {
    int bit_idx = bitmap_scan(&m_pool->pool_bitmap, 1);
    if(bit_idx == -1) {
        return NULL;
    }
    bitmap_set(&m_pool->pool_bitmap, bit_idx, 1);
    uint32_t page_phyaddr = ((bit_idx * PG_SIZE) + m_pool->phy_addr_start);
    return (void*)page_phyaddr;
}

// 在页表中添加虚拟地址_vaddr与物理地址page-phyaddr映射 --- 就是在页表项和页表中填上相应的物理地址
static void page_table_add(void* _vaddr, void* _page_phyaddr) {
    uint32_t vaddr = (uint32_t)_vaddr, page_phyaddr = (uint32_t)_page_phyaddr;
    uint32_t* pde = pde_ptr(vaddr);
    uint32_t* pte = pte_ptr(vaddr);
    
    if(*pde & 0x00000001) { // 页目录项存在
        ASSERT(!(*pte & 0x00000001));

        if(!(*pte & 0x00000001)) {
            *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
        } else {
            PANIC("pte repeat");
        }
    } else { // 页目录项不存在
        // 页表中用到的页框一律从内核空间分配        
        uint32_t pde_phyaddr = (uint32_t)palloc(&kernel_pool);

        *pde = (pde_phyaddr | PG_US_U | PG_RW_W | PG_P_1);

        memset((void*)((int)pte & 0xfffff000), 0, PG_SIZE);

        ASSERT(!(*pte & 0x00000001));
        *pte = (page_phyaddr | PG_US_U | PG_RW_W | PG_P_1);
    }
}

  //分配 pg_cnt 个页空间, 成功则返回起始虚拟地址, 失败时返回 NULL
void* malloc_page(enum pool_flags pf, uint32_t pg_cnt) {
    // 内核和用户空间各约16MB的空间（内存一共配置了32MB大小），保守起见，用15MB来限制
    ASSERT(pg_cnt > 0 && pg_cnt <3840);

    //1. 通过 vaddr_get 在虚拟内存池中申请虚拟地址
    //2. 通过palloc在物理内存池中申请物理页
    //3. 通过pege_table_add 将以上得到的虚拟地址和物理地址在页表中完成映射
    // 注意，1.2.两步得到的虚拟地址都是页对齐的，最后12位都是0
    void* vaddr_start = vaddr_get(pf, pg_cnt);

    if( vaddr_start == NULL) {
        return NULL;
    }

    uint32_t vaddr = (uint32_t) vaddr_start, cnt = pg_cnt;
    struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;

    //虚拟地址连续，但是物理地址可以不连续，所以逐个做映射
    while(cnt-- > 0){
        void* page_phyaddr = palloc(mem_pool);

        //分配物理页
        if(page_phyaddr == NULL) {//如果内存池中没有空间了，就会返回null
            //TODO. 失败时要将全部的页滚回
             return NULL;
        }

        page_table_add((void*)vaddr, page_phyaddr);//物理页与虚拟页做匹配
        vaddr += PG_SIZE; //vaddr写到一个虚拟页面，虚拟地址是连续的，所以可以直接像这样加
    }
    return vaddr_start;
}


// 从内核物理内存中申请 pg_cnt页
//成功则返回对应的页的起始虚拟地址
// 从内核物理内存池中申请 1 页内存
// 成功则返回其虚拟地址, 失败则返回 NULL
void* get_kernel_pages(uint32_t pg_cnt) {
    lock_acquire(&kernel_pool.lock);
    void* vaddr = malloc_page(PF_KERNEL, pg_cnt);
    if(vaddr != NULL) {
        memset(vaddr, 0, pg_cnt*PG_SIZE);
    }
    lock_release(&kernel_pool.lock);
    return vaddr;
}

// 在用户空间中申请 4k 内存, 并返回其虚拟地址
void* get_user_pages(uint32_t pg_cnt) {
    lock_acquire(&user_pool.lock);
    void* vaddr = malloc_page(PF_USER, pg_cnt);
    memset(vaddr, 0, pg_cnt*PG_SIZE);
    lock_release(&user_pool.lock);
    return vaddr;
}

// 将地址 vaddr 与 pf 池中的物理地址关联 与 get_kernel_pages get_user_pages函数相比，这个函数能够指定关联的虚拟地址
// 这个函数用来分配用户线程的3特权级栈，因为3特权级栈的栈顶是分配好的，详见 process.h中的宏
void* get_a_page(enum pool_flags pf, uint32_t vaddr) {
    struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
    lock_acquire(&mem_pool->lock);
    // 先将虚拟地址对应的位图置 1
    struct task_struct* cur = running_thread();
    int32_t bit_idx = -1;
    
    if(cur->pgdir != NULL && pf == PF_USER) {
        // 若当前是用户进程申请用户内存, 就修改用户进程自己的虚拟地址位图
        bit_idx = (vaddr - cur->userprog_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&cur->userprog_vaddr.vaddr_bitmap, bit_idx, 1);
    } else if(cur->pgdir == NULL && pf == PF_KERNEL) {
        // 如果是内核线程申请内核内存, 就修改 kernel_vaddr
        bit_idx = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        ASSERT(bit_idx > 0);
        bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx, 1);
    } else {
        PANIC("get_a_page: not allow kernel alloc userspace or user alloc kernelspace by get_a_page");
    }

    void* page_phyaddr = palloc(mem_pool);
    if(page_phyaddr == NULL) {
        return NULL;
    }
    page_table_add((void*)vaddr, page_phyaddr);
    lock_release(&mem_pool->lock);
    return (void*)vaddr;
}

/* 安装1页大小的vaddr,专门针对fork时虚拟地址位图无须操作的情况 */
void* get_a_page_without_opvaddrbitmap(enum pool_flags pf, uint32_t vaddr) {
   struct pool* mem_pool = pf & PF_KERNEL ? &kernel_pool : &user_pool;
   lock_acquire(&mem_pool->lock);
   void* page_phyaddr = palloc(mem_pool);
   if (page_phyaddr == NULL) {
      lock_release(&mem_pool->lock);
      return NULL;
   }
   page_table_add((void*)vaddr, page_phyaddr); 
   lock_release(&mem_pool->lock);
   return (void*)vaddr;
}

//将虚拟地址转化为物理地址
//  对vaddr相应的pte指针解引用，然后取结果的前二十位， 再加上vaddr后十二位即可得到物理地址
uint32_t addr_v2p(uint32_t vaddr) {
    uint32_t* pte = pte_ptr(vaddr);
    return ((*pte & 0xfffff000) + (vaddr & 0x00000fff));
}

// 返回 arena 中第 idx 个内存块的地址
static struct mem_block* arena2block(struct arena* a, uint32_t idx) {
    //要跳过arena的元信息
  return (struct mem_block*)((uint32_t)a + sizeof(struct arena) + idx * a->desc->block_size);
}

// 返回内存块b所在的arena地址
static struct arena* block2arena(struct mem_block* b) {
   return (struct arena*)((uint32_t)b & 0xfffff000);
}

/* 在堆中申请size字节内存 */
void* sys_malloc(uint32_t size) {
   enum pool_flags PF;
   struct pool* mem_pool;
   uint32_t pool_size;
   struct mem_block_desc* descs;
   struct task_struct* cur_thread = running_thread();

/* 判断用哪个内存池*/
   if (cur_thread->pgdir == NULL) {     // 若为内核线程
      PF = PF_KERNEL; 
      pool_size = kernel_pool.pool_size;
      mem_pool = &kernel_pool;
      descs = k_block_descs;
   } else {				      // 用户进程pcb中的pgdir会在为其分配页表时创建
      PF = PF_USER;
      pool_size = user_pool.pool_size;
      mem_pool = &user_pool;
      descs = cur_thread->u_block_desc;
   }

   /* 若申请的内存不在内存池容量范围内则直接返回NULL */
   if (!(size > 0 && size < pool_size)) {
      return NULL;
   }
   struct arena* a;
   struct mem_block* b;	
   lock_acquire(&mem_pool->lock);

/* 超过最大内存块1024, 就分配页框 */
   if (size > 1024) {
      uint32_t page_cnt = DIV_ROUND_UP(size + sizeof(struct arena), PG_SIZE);    // 向上取整需要的页框数

      a = malloc_page(PF, page_cnt);

      if (a != NULL) {
        memset(a, 0, page_cnt * PG_SIZE);	 // 将分配的内存清0  

        /* 对于分配的大块页框,将desc置为NULL, cnt置为页框数,large置为true */
        a->desc = NULL;
        a->cnt = page_cnt;
        a->large = true;
        lock_release(&mem_pool->lock);
        return (void*)(a + 1);		 // 跨过arena大小，把剩下的内存返回
      } else { 
        lock_release(&mem_pool->lock);
        return NULL; 
      }
   } else {    // 若申请的内存小于等于1024,可在各种规格的mem_block_desc中去适配
      uint8_t desc_idx;
      
      /* 从内存块描述符中匹配合适的内存块规格 */
      for (desc_idx = 0; desc_idx < DESC_CNT; desc_idx++) {
        if (size <= descs[desc_idx].block_size) {  // 从小往大后,找到后退出
            break;
        }
      }

   /* 若mem_block_desc的free_list中已经没有可用的mem_block,
    * 就创建新的arena提供mem_block */
      if (list_empty(&descs[desc_idx].free_list)) {
        a = malloc_page(PF, 1);       // 分配1页框做为arena
        if (a == NULL) {
            lock_release(&mem_pool->lock);
            return NULL;
        }
        memset(a, 0, PG_SIZE);

        /* 对于分配的小块内存,将desc置为相应内存块描述符, 
        * cnt置为此arena可用的内存块数,large置为false */
        a->desc = &descs[desc_idx];
        a->large = false;
        a->cnt = descs[desc_idx].blocks_per_arena;
        uint32_t block_idx;

        enum intr_status old_status = intr_disable();

        /* 开始将arena拆分成内存块,并添加到内存块描述符的free_list中 */
        for (block_idx = 0; block_idx < descs[desc_idx].blocks_per_arena; block_idx++) {
            b = arena2block(a, block_idx);
            ASSERT(!elem_find(&a->desc->free_list, &b->free_elem));
            list_append(&a->desc->free_list, &b->free_elem);	
        }
        intr_set_status(old_status);
    }    

   /* 开始分配内存块 */
      b = elem2entry(struct mem_block, free_elem, list_pop(&(descs[desc_idx].free_list)));
      memset(b, 0, descs[desc_idx].block_size);

      a = block2arena(b);  // 获取内存块b所在的arena
      a->cnt--;		   // 将此arena中的空闲内存块数减1
      lock_release(&mem_pool->lock);
      return (void*)b;
   }
}

//释放物理地址为pg_phy_addr的一个自然页，仅仅将对应的位图置为0即可
void pfree(uint32_t pg_phy_addr) {
    struct pool* mem_pool;
    uint32_t bit_idx = 0;
    if(pg_phy_addr >= user_pool.phy_addr_start) {//是用户物理内存池
        mem_pool = &user_pool;
        bit_idx = (pg_phy_addr - user_pool.phy_addr_start) / PG_SIZE;
    } else {
        mem_pool = &kernel_pool;
        bit_idx = (pg_phy_addr - kernel_pool.phy_addr_start) / PG_SIZE;
    }
    bitmap_set(&mem_pool->pool_bitmap, bit_idx, 0);//将对应的一页的位图置零
}

//去掉页表中虚拟地址vaddr的映射，将对应pte的p位置为0即可；
//最后一定要跟新tlb缓存
static void page_table_pte_remove(uint32_t vaddr)  {
    uint32_t* pte = pte_ptr(vaddr);
    *pte &= ~PG_P_1;	// 将页表项pte的P位置0
    asm volatile ("invlpg %0"::"m" (vaddr):"memory");    //更新tlb!!,一定得跟新！！
}

//在虚拟地址池中释放以_Vaddr起始的连续pg_cnt个虚拟页地址
//也只是简单地将对应的位图置为0即可，只不过虚拟地址是连续的，所以可以连续释放
// 而pfree释放的是物理内存，物理内存可以不连续，所以只能一页一页地释放
static void vaddr_remove(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt) {
    uint32_t bit_idx_start = 0, vaddr = (uint32_t)_vaddr, cnt = 0;

    if(pf == PF_KERNEL) {
        bit_idx_start = (vaddr - kernel_vaddr.vaddr_start) / PG_SIZE;
        while(cnt < pg_cnt) {
            bitmap_set(&kernel_vaddr.vaddr_bitmap, bit_idx_start + cnt++, 0);
        }
    } else {
        struct task_struct* cur_thread = running_thread();
        bit_idx_start = (vaddr - cur_thread->userprog_vaddr.vaddr_start) / PG_SIZE;
        while(cnt < pg_cnt) {
            bitmap_set(&cur_thread->userprog_vaddr.vaddr_bitmap,bit_idx_start + cnt++, 0);
        }
    }
}

/* 释放以虚拟地址vaddr为起始的cnt个物理页框 */
void mfree_page(enum pool_flags pf, void* _vaddr, uint32_t pg_cnt) {
    uint32_t pg_phy_addr;
    uint32_t vaddr = (int32_t)_vaddr,page_cnt = 0;
    ASSERT(pg_cnt >= 1 && vaddr % PG_SIZE == 0);
    pg_phy_addr = addr_v2p(vaddr);

/* 确保待释放的物理内存在低端1M+1k大小的页目录+1k大小的页表地址范围外 */
    ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= 0x102000);

    //判断物理地址属于物理池还是用户池
    if (pg_phy_addr >= user_pool.phy_addr_start) {//用户池处理
        vaddr -= PG_SIZE;
        //循环释放物理地址池，并将对应的pte的p位置1
        while( page_cnt < pg_cnt ) {
            vaddr += PG_SIZE;
            pg_phy_addr = addr_v2p(vaddr);

            //确保物理地址属于用户物理内存池 
            ASSERT((pg_phy_addr % PG_SIZE) == 0 && pg_phy_addr >= user_pool.phy_addr_start);

            // 先将对应的物理页框(只是一页)归还到内存池
            pfree(pg_phy_addr);

            /* 再从页表中清除此虚拟地址所在的页表项pte */
            page_table_pte_remove(vaddr);

            page_cnt++;
        }
        // 因为虚拟地址是连续的，所以可以不用循环释放
        vaddr_remove(pf, _vaddr, pg_cnt);
    } else {//kernel内存池
        vaddr -= PG_SIZE;	      
        while (page_cnt < pg_cnt) {
            vaddr += PG_SIZE;
            pg_phy_addr = addr_v2p(vaddr);
            // 确保待释放的物理内存只属于内核物理内存池 
            ASSERT((pg_phy_addr % PG_SIZE) == 0 && \
                pg_phy_addr >= kernel_pool.phy_addr_start && \
                pg_phy_addr < user_pool.phy_addr_start);
            
            // 先将对应的物理页框归还到内存池 
            pfree(pg_phy_addr);

            /* 再从页表中清除此虚拟地址所在的页表项pte */
            page_table_pte_remove(vaddr);

            page_cnt++;
        }
   // 清空虚拟地址的位图中的相应位
        vaddr_remove(pf, _vaddr, pg_cnt);
    }

}

//系统调用子处理程序sys_free() 可以对内存块进行更精细的控制
// 基于arena和mfree
//sys_free()是内存释放的统一接口
void sys_free(void* ptr) {
    ASSERT(ptr != NULL);
    if(ptr != NULL) {
        enum pool_flags PF;
        struct pool* mem_pool;

        //判断是进程还是线程
        if(running_thread()->pgdir == NULL) {
            ASSERT((uint32_t)ptr >= K_HEAP_START);
            PF = PF_KERNEL;
            mem_pool = &kernel_pool;
        } else {
            PF = PF_USER;
            mem_pool = &user_pool;
        }

        lock_acquire(&mem_pool->lock);
        struct mem_block* b = ptr;
        struct arena*  a= block2arena(b);

        ASSERT(a->large == 0 || a->large == 1);

        if(a->desc == NULL && a->large == true) {
            //大于1024字节的是直接按页分配的，所以在回收时，也使用mfree_page回收相应的页框即可
            mfree_page(PF,a,a->cnt);
        } else{
            //先将内存块回收到free_list
            list_append(&a->desc->free_list, &b->free_elem);

            //再判断此arena中的内存块是否都空闲，如果是的话就是放arena对应的物理页框，但在这之前，要将对饮描述符的空闲链表中的对应元素删除掉
            if (++a->cnt == a->desc->blocks_per_arena) {
                uint32_t block_idx;
                for (block_idx = 0; block_idx < a->desc->blocks_per_arena; block_idx++) {
                    struct mem_block*  b = arena2block(a, block_idx);
                    ASSERT(elem_find(&a->desc->free_list, &b->free_elem));
                    list_remove(&b->free_elem);//将内存块从对应的描述符的空闲链表中删除
                }
                mfree_page(PF, a, 1); 
            } 
        }
        lock_release(&mem_pool->lock);
    }
}

// 初始化内存池
static void mem_pool_init(uint32_t all_mem) {
    put_str("mem_pool_init_start\n");
    //page_table_size是目前页表占用的大小
    // 目前页的数量 = 1页页目录表 + 第0和768个页目录项指向同一个页表 + 第769 ~ 1022个页目录项共指向254个也页表，所以一共256个页
    uint32_t page_table_size = PG_SIZE * 256;
    uint32_t used_mem = page_table_size +  0x100000;//0x100000 为低端 1MB 内存
    uint32_t free_mem = all_mem - used_mem;

    uint16_t all_free_pages = free_mem / PG_SIZE;//所有空闲页的数量
    uint16_t kernel_free_pages = all_free_pages / 2; //内核与用户能够使用的内存对半分。
    uint16_t user_free_pages = all_free_pages - kernel_free_pages;

    uint32_t kbm_length = kernel_free_pages / 8; // 内核物理内存位图的长度
    uint32_t ubm_length = user_free_pages / 8; // 用户物理内存位图的长度

    uint32_t kp_start = used_mem;//内核内存池的起始地址
    uint32_t up_start = kp_start + kernel_free_pages * PG_SIZE;//用户内存池的起始地址
    
    kernel_pool.phy_addr_start = kp_start;
    user_pool.phy_addr_start = up_start;

    kernel_pool.pool_size = kernel_free_pages * PG_SIZE; //内核池（物理）大小  单位：字节
    user_pool.pool_size = user_free_pages * PG_SIZE; //用户池（物理）大小
    
    kernel_pool.pool_bitmap.btmp_bytes_len = kbm_length;
    user_pool.pool_bitmap.btmp_bytes_len = ubm_length;

    //内核内存池的位图在MEM_BITMAP_BASE（0x9a000）处
    kernel_pool.pool_bitmap.bits = (void*)MEM_BITMAP_BASE;
    //用户内存池的位图紧跟在内核内存池的位图之后
    user_pool.pool_bitmap.bits = (void*)(MEM_BITMAP_BASE + kbm_length);

    // 输出内存池信息
    put_str("    kernel_pool_bitmap_start:");
    put_int((int)kernel_pool.pool_bitmap.bits);
    put_str(" kernel_pool_phy_addr_start:");
    put_int(kernel_pool.phy_addr_start);
    put_str("\n");
    put_str("    user_pool_bitmap_start:");
    put_int((int)user_pool.pool_bitmap.bits);
    put_str(" user_pool_phy_addr_start:");
    put_int(user_pool.phy_addr_start);
    put_str("\n");

    //将位图置零
    bitmap_init(&kernel_pool.pool_bitmap);
    bitmap_init(&user_pool.pool_bitmap);

    //十一章新增: 锁的初始化
    lock_init(&kernel_pool.lock);
    lock_init(&user_pool.lock);


    //初始化内核虚拟地址的位图，按实际物理内存大小生成数组
    // 要和内核内存池的大小一致
    kernel_vaddr.vaddr_bitmap.btmp_bytes_len = kbm_length;
   


    //虚拟地址池的管理结构的位图的物理地址在上面两个位图之后
    kernel_vaddr.vaddr_bitmap.bits = (void*) (MEM_BITMAP_BASE + kbm_length + ubm_length);
    //虚拟地址的起始地址为0x100000,还没有将其与物理地址对应起来！！！
    kernel_vaddr.vaddr_start = K_HEAP_START;
    
    bitmap_init(&kernel_vaddr.vaddr_bitmap);

    put_str("    mem_pool_init done\n");
}


void block_desc_init(struct mem_block_desc* desc_array) {				   
   uint16_t desc_idx, block_size = 16;

   /* 初始化每个mem_block_desc描述符 */
   for (desc_idx = 0; desc_idx < DESC_CNT; desc_idx++) {
      desc_array[desc_idx].block_size = block_size;

      /* 初始化arena中的内存块数量 */
      desc_array[desc_idx].blocks_per_arena = (PG_SIZE - sizeof(struct arena)) / block_size;	  

      list_init(&desc_array[desc_idx].free_list);

      block_size *= 2;         //下标越低的内存块描述符，其表示的内存块容量越小
   }
}

void mem_init() {
    put_str("mem_init start\n");
    //实际内存容量在LOADER.S中算出并存在了0xb00物理地址处，因为定义了dd，所以应该用32位指针去转换它，然后dereference给C语言的32位无符号整型。
    uint32_t mem_bytes_total = (*(uint32_t*)(0xb00));
    mem_pool_init(mem_bytes_total);
    block_desc_init(k_block_descs);// 初始化内核内存块描述符
    put_str("mem_bytes_total: 0x"); put_int(mem_bytes_total);put_str("\n");
    put_str("mem_init done\n");
}

#include "file.h"
#include "fs.h"
#include "super_block.h"
#include "inode.h"
#include "stdio-kernel.h"
#include "memory.h"
#include "debug.h"
#include "interrupt.h"
#include "string.h"
#include "thread.h"
#include "global.h"

#define DEFAULT_SECS 1

/* 文件表 */
struct file file_table[MAX_FILE_OPEN];

/* 从文件表file_table中获取一个空闲位,成功返回下标,失败返回-1 */
int32_t get_free_slot_in_global(void) {
   //预留3个文件结构给标准输入、标准输出和标准错误
   uint32_t fd_idx = 3;
   while (fd_idx < MAX_FILE_OPEN) {
      if (file_table[fd_idx].fd_inode == NULL) {
	      break;
      }
      fd_idx++;
   }
   if (fd_idx == MAX_FILE_OPEN) {
      printk("exceed max open files\n");
      return -1;
   }
   return fd_idx;
}

/* 将全局描述符下标（filetable的下标）安装到进程或线程自己的文件描述符数组fd_table中,
 * 成功返回下标,失败返回-1 */
int32_t pcb_fd_install(int32_t globa_fd_idx) {
   struct task_struct* cur = running_thread();
   uint8_t local_fd_idx = 3; // 跨过stdin,stdout,stderr
   while (local_fd_idx < MAX_FILES_OPEN_PER_PROC) {
      if (cur->fd_table[local_fd_idx] == -1) {	// -1表示free_slot,表示这个文件描述符是空的，可用
        cur->fd_table[local_fd_idx] = globa_fd_idx; //在进程fd_table中的一个位置存储filetable的相应下标
        break;
      }
      local_fd_idx++;
   }
   if (local_fd_idx == MAX_FILES_OPEN_PER_PROC) {
      printk("exceed max open files_per_proc\n");
      return -1;
   }
   return local_fd_idx;
}

/* 在part->inode_bitmap中分配一个i结点,返回i结点号 */
int32_t inode_bitmap_alloc(struct partition* part) {
   int32_t bit_idx = bitmap_scan(&part->inode_bitmap, 1);
   if (bit_idx == -1) {
      return -1;
   }
   bitmap_set(&part->inode_bitmap, bit_idx, 1);
   return bit_idx;
}
   
/* 在part->block_bitmap中分配1个空闲扇区,返回其扇区地址 */
int32_t block_bitmap_alloc(struct partition* part) {
   int32_t bit_idx = bitmap_scan(&part->block_bitmap, 1);
   if (bit_idx == -1) {
      return -1;
   }
   bitmap_set(&part->block_bitmap, bit_idx, 1);
   /* 和inode_bitmap_malloc不同,此处返回的不是位图索引,而是具体可用的扇区地址 */
   return (part->sb->data_start_lba + bit_idx);
} 

/* 将内存中bitmap第bit_idx位所在的512字节同步到硬盘 
    参数：
    part -- 硬盘分区
    bit_idx -- 
    btmp -- 表示位图的种类，是inode数组位图，还是空闲块位图
*/
void bitmap_sync(struct partition* part, uint32_t bit_idx, uint8_t btmp_type) {
   uint32_t off_sec = bit_idx / 4096;  // 本i结点索引相对于位图的扇区偏移量
   uint32_t off_size = off_sec * BLOCK_SIZE;  // 本i结点索引相对于位图的字节偏移量
   uint32_t sec_lba;
   uint8_t* bitmap_off;

/* 需要被同步到硬盘的位图只有inode_bitmap和block_bitmap */
   switch (btmp_type) {
    case INODE_BITMAP:
	 sec_lba = part->sb->inode_bitmap_lba + off_sec; //这是内存中记录硬盘上的对应的硬盘块起始地址
	 bitmap_off = part->inode_bitmap.bits + off_size;//这是在内存中的位图
	 break;

    case BLOCK_BITMAP: 
	 sec_lba = part->sb->block_bitmap_lba + off_sec;//这是内存中记录硬盘上的对应的硬盘块起始地址
	 bitmap_off = part->block_bitmap.bits + off_size;//这是在内存中的位图
	 break;
   }
   ide_write(part->my_disk, sec_lba, bitmap_off, 1);//将内存中的位图写道磁盘上
}

/* 创建文件,若成功则返回文件描述符,否则返回-1 
------------------------------------------------------------
   parent_dir : 父目录
   filename： 创建的文件名称
   flag：创建的文件的模式
   ------------------------------------
   返回值 ： 若成功则进程的文件描述符，即pcb_fdtable中的下标，否则返回-1
   且如果成功则有如下副作用：
      1. 内存：将inode添加到内存中的inode队列中
      2. 内存：在进程的本地描述符数组中添加新的描述符并指向全局文件描述符
      3. 硬盘：相应的inode、bitmap等数据都已经同步到磁盘
*/
int32_t file_create(struct dir* parent_dir, char* filename, uint8_t flag) {
   /* 后续操作的公共缓冲区 */
   void* io_buf = sys_malloc(1024);//一般情况下，都是读写一个硬盘的扇区（512字节），考虑到有跨区的情况，这里申请2个扇区的缓冲区。
   if (io_buf == NULL) {
      printk("in file_creat: sys_malloc for io_buf failed\n");
      return -1;
   }

   uint8_t rollback_step = 0;	       // 用于操作失败时回滚各资源状态

   /* 为新文件分配inode */
   int32_t inode_no = inode_bitmap_alloc(cur_part); //挂载文件系统时，mount_partition()已将cur_part初始化
   if (inode_no == -1) {
      printk("in file_creat: allocate inode failed\n");
      return -1;
   }

/* 此inode要从堆中申请内存,不可生成局部变量(函数退出时会释放)
 * 因为file_table数组中的文件描述符的inode指针要指向它.*/
   struct inode* new_file_inode = (struct inode*)sys_malloc(sizeof(struct inode)); 
   if (new_file_inode == NULL) {
      printk("file_create: sys_malloc for inode failded\n");//内存分配失败，需要回滚
      rollback_step = 1;
      goto rollback;//回滚至，case1，仅仅需要将inode位图重置就可以了
   }
   inode_init(inode_no, new_file_inode);	    // 初始化i结点

   /* 返回的是file_table数组的下标 */
   int fd_idx = get_free_slot_in_global();
   if (fd_idx == -1) {
      printk("exceed max open files\n");//file_table中没有空位，需要回滚
      rollback_step = 2;
      goto rollback; //回滚至case2，将inode项删除，并且将inode位图重置
   }

   //在fdtable中安装这个文件inode
   file_table[fd_idx].fd_inode = new_file_inode;
   file_table[fd_idx].fd_pos = 0;
   file_table[fd_idx].fd_flag = flag;
   file_table[fd_idx].fd_inode->write_deny = false;

   struct dir_entry new_dir_entry;
   memset(&new_dir_entry, 0, sizeof(struct dir_entry));

   create_dir_entry(filename, inode_no, FT_REGULAR, &new_dir_entry);	// create_dir_entry只是内存操作不出意外,不会返回失败

/* 同步内存数据到硬盘 */
   /* a 在目录parent_dir下安装目录项new_dir_entry, 写入硬盘后返回true,否则false */
   if (!sync_dir_entry(parent_dir, &new_dir_entry, io_buf)) {
      printk("sync dir_entry to disk failed\n");
      rollback_step = 3;
      goto rollback;
   }

   memset(io_buf, 0, 1024);
   /* b sync_dir_entry会改变parent_dir的信息，所以将父目录对应的i结点的内容同步到硬盘 */
   inode_sync(cur_part, parent_dir->inode, io_buf);

   memset(io_buf, 0, 1024);
   /* c 将新创建文件的i结点内容同步到硬盘 */
   inode_sync(cur_part, new_file_inode, io_buf);

   /* d 将inode_bitmap位图同步到硬盘 */
   bitmap_sync(cur_part, inode_no, INODE_BITMAP);

   /* e 将创建的文件i结点添加到open_inodes链表 */
   list_push(&cur_part->open_inodes, &new_file_inode->inode_tag);
   new_file_inode->i_open_cnts = 1;

   sys_free(io_buf);
   return pcb_fd_install(fd_idx);//在进程的本地描述符数组中添加新的描述符指向全局文件描述符

/*创建文件需要创建相关的多个资源,若某步失败则会执行到下面的回滚步骤 */
rollback:
   switch (rollback_step) {
      case 3:
	 /* 失败时,将file_table中的相应位清空 */
	 memset(&file_table[fd_idx], 0, sizeof(struct file)); 
      case 2:
	 sys_free(new_file_inode);
      case 1:
	 /* 如果新文件的i结点创建失败,之前位图中分配的inode_no也要恢复 */
	 bitmap_set(&cur_part->inode_bitmap, inode_no, 0);
	 break;
   }
   sys_free(io_buf);
   return -1;
}
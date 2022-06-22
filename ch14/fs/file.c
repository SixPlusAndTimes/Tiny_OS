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

/* 打开编号为inode_no的inode对应的文件,若成功则返回文件描述符,否则返回-1 
--------------------------------------------------------------------------
原理：调用inode_open将filetable中某个元素的inode指针指向内存中的inode数据结构
然后调用pcb_fd_install将全局文件描述符安装在进程的局部文件描述符表中
*/
int32_t file_open(uint32_t inode_no, uint8_t flag) {
   int fd_idx = get_free_slot_in_global();
   if (fd_idx == -1) {
      printk("exceed max open files\n");
      return -1;
   }
   file_table[fd_idx].fd_inode = inode_open(cur_part, inode_no);
   file_table[fd_idx].fd_pos = 0;	     // 每次打开文件,要将fd_pos还原为0,即让文件内的指针指向开头
   file_table[fd_idx].fd_flag = flag;
   bool* write_deny = &file_table[fd_idx].fd_inode->write_deny; 

   if (flag & O_WRONLY || flag & O_RDWR) {	// 只要是关于写文件,判断是否有其它进程正写此文件
						// 若是读文件,不考虑write_deny
   /* 以下进入临界区前先关中断 */
      enum intr_status old_status = intr_disable();
      if (!(*write_deny)) {    // 若当前没有其它进程写该文件,将其占用.
         *write_deny = true;   // 置为true,避免多个进程同时写此文件
         intr_set_status(old_status);	  // 恢复中断
      } else {		// 直接失败返回
         intr_set_status(old_status);
         printk("file can`t be write now, try again later\n");
         return -1;
      }
   }  // 若是读文件或创建文件,不用理会write_deny,保持默认
   return pcb_fd_install(fd_idx);
}

/* 关闭文件 
------------------------
原理 ： 将全局描述符表中的对应项重置为初始值，调用inode_close对相应的inode执行一次关闭
*/
int32_t file_close(struct file* file) {
   if (file == NULL) {
      return -1;
   }
   file->fd_inode->write_deny = false;
   inode_close(file->fd_inode);
   file->fd_inode = NULL;   // 使文件结构可用
   return 0;
}


/* 把buf中的count个字节写入file,成功则返回写入的字节数,失败则返回-1 
---------------------------------------------------------------------
参数 ： 
   - file ： 文件
   - buf ： 数据缓冲区，存放要写入文件的数据
   - count ： 要写入文件的字节数
----------------------------------------------------------------------
*/
int32_t file_write(struct file* file, const void* buf, uint32_t count) {
   if ((file->fd_inode->i_size + count) > (BLOCK_SIZE * 140))	{   // 文件目前最大只支持512*140=71680字节
      printk("exceed max file_size 71680 bytes, write file failed\n");
      return -1;
   }
   uint8_t* io_buf = sys_malloc(BLOCK_SIZE);
   if (io_buf == NULL) {
      printk("file_write: sys_malloc for io_buf failed\n");
      return -1;
   }
   uint32_t* all_blocks = (uint32_t*)sys_malloc(BLOCK_SIZE + 48);	  //(128个间接块+12个直接块)*4 .用来记录文件所有的块地址,注意是地址
   if (all_blocks == NULL) {
      printk("file_write: sys_malloc for all_blocks failed\n");
      return -1;
   }

   const uint8_t* src = buf;	    // 用src指向buf中待写入的数据 
   uint32_t bytes_written = 0;	    // 用来记录已写入数据大小
   uint32_t size_left = count;	    // 用来记录未写入数据大小
   int32_t block_lba = -1;	    // 块地址
   uint32_t block_bitmap_idx = 0;   // 用来记录block对应于block_bitmap中的索引,做为参数传给bitmap_sync
   uint32_t sec_idx;	      // 用来索引扇区
   uint32_t sec_lba;	      // 扇区地址
   uint32_t sec_off_bytes;    // 扇区内字节偏移量
   uint32_t sec_left_bytes;   // 扇区内剩余字节量
   uint32_t chunk_size;	      // 每次写入硬盘的数据块大小
   int32_t indirect_block_table;      // 用来获取一级间接表地址
   uint32_t block_idx;		      // 块索引

   /* 判断文件是否是第一次写,如果是,先为其分配一个块 */
   if (file->fd_inode->i_sectors[0] == 0) {
      //文件第一次写，先为其分配一个块
      block_lba = block_bitmap_alloc(cur_part);
      if (block_lba == -1) {
         printk("file_write: block_bitmap_alloc failed\n");
         return -1;
      }
      file->fd_inode->i_sectors[0] = block_lba;

      /* 每分配一个块就将位图同步到硬盘 */
      block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
      ASSERT(block_bitmap_idx != 0);
      bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
   }

   /* 写入count个字节前,该文件已经占用的块数 */
   uint32_t file_has_used_blocks = file->fd_inode->i_size / BLOCK_SIZE + 1;

   /* 存储count字节后该文件将占用的块数 */
   uint32_t file_will_use_blocks = (file->fd_inode->i_size + count) / BLOCK_SIZE + 1;
   ASSERT(file_will_use_blocks <= 140);

   /* 通过此增量判断是否需要分配扇区,如增量为0,表示原扇区够用 */
   uint32_t add_blocks = file_will_use_blocks - file_has_used_blocks;

/* 开始将文件所有块地址收集到all_blocks,(系统中块大小等于扇区大小)
 * 后面都统一在all_blocks中获取写入扇区地址 */
   if (add_blocks == 0) { 
   /* 在同一扇区内写入数据,不涉及到分配新扇区 */
      if (file_has_used_blocks <= 12 ) {	// 文件数据量将在12块之内，则原有块地址为直接块
         block_idx = file_has_used_blocks - 1;  // 指向最后一个已有数据的扇区
         all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];
      } else { 
            /* 未写入新数据之前已经占用了间接块,需要将间接块地址读进来 */
         ASSERT(file->fd_inode->i_sectors[12] != 0);
               indirect_block_table = file->fd_inode->i_sectors[12];
         ide_read(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);
      }
   } else {
   /* 若有增量,便涉及到分配新扇区及是否分配一级间接块表,下面要分三种情况处理 */
   /* 第一种情况:12个直接块够用，那么直接分配所需的块并把块地址写入i_sector数组中即可*/
      if (file_will_use_blocks <= 12 ) {
         /* 先将有剩余空间的可继续用的扇区地址写入all_blocks */
         block_idx = file_has_used_blocks - 1;
         ASSERT(file->fd_inode->i_sectors[block_idx] != 0);
         all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];

         /* 再将未来要用的扇区分配好后写入all_blocks */
         block_idx = file_has_used_blocks;      // 指向第一个要分配的新扇区
         while (block_idx < file_will_use_blocks) {
            block_lba = block_bitmap_alloc(cur_part);
            if (block_lba == -1) {
               printk("file_write: block_bitmap_alloc for situation 1 failed\n");
               return -1;
            }

            /* 写文件时,不应该存在块未使用但已经分配扇区的情况,当文件删除时,就会把块地址清0 */
            ASSERT(file->fd_inode->i_sectors[block_idx] == 0);     // 确保尚未分配扇区地址
            file->fd_inode->i_sectors[block_idx] = all_blocks[block_idx] = block_lba;

            /* 每分配一个块就将位图同步到硬盘 */
            block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

            block_idx++;   // 下一个分配的新扇区
         }
      } else if (file_has_used_blocks <= 12 && file_will_use_blocks > 12) { 
         /* 第二种情况: 旧数据在12个直接块内,新数据将使用间接块*/

         /* 先将有剩余空间的可继续用的扇区地址收集到all_blocks */
         block_idx = file_has_used_blocks - 1;      // 指向旧数据所在的最后一个扇区
         all_blocks[block_idx] = file->fd_inode->i_sectors[block_idx];

         /* 创建一级间接块表 */
         block_lba = block_bitmap_alloc(cur_part);
         if (block_lba == -1) {
            printk("file_write: block_bitmap_alloc for situation 2 failed\n");
            return -1;
         }

         ASSERT(file->fd_inode->i_sectors[12] == 0);  // 确保一级间接块表未分配
         /* 分配一级间接块索引表 */
         indirect_block_table = file->fd_inode->i_sectors[12] = block_lba;

         block_idx = file_has_used_blocks;	// 第一个未使用的块,即本文件最后一个已经使用的直接块的下一块
         while (block_idx < file_will_use_blocks) {
            block_lba = block_bitmap_alloc(cur_part);
            if (block_lba == -1) {
               printk("file_write: block_bitmap_alloc for situation 2 failed\n");
               return -1;
            }

            if (block_idx < 12) {      // 新创建的0~11块直接存入all_blocks数组
               ASSERT(file->fd_inode->i_sectors[block_idx] == 0);      // 确保尚未分配扇区地址
               file->fd_inode->i_sectors[block_idx] = all_blocks[block_idx] = block_lba;
            } else {     // 间接块只写入到all_block数组中,待全部分配完成后一次性同步到硬盘
               all_blocks[block_idx] = block_lba;
            }

            /* 每分配一个块就将位图同步到硬盘 */
            block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);

            block_idx++;   // 下一个新扇区
         }
         ide_write(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);      // 同步一级间接块表到硬盘
      } else if (file_has_used_blocks > 12) {
         /* 第三种情况:新数据占据间接块*/
         ASSERT(file->fd_inode->i_sectors[12] != 0); // 已经具备了一级间接块表
         indirect_block_table = file->fd_inode->i_sectors[12];	 // 获取一级间接表地址

         /* 已使用的间接块也将被读入all_blocks,无须单独收录 */
         ide_read(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1); // 获取所有间接块地址

         block_idx = file_has_used_blocks;	  // 第一个未使用的间接块,即已经使用的间接块的下一块
         while (block_idx < file_will_use_blocks) {
            block_lba = block_bitmap_alloc(cur_part);
            if (block_lba == -1) {
               printk("file_write: block_bitmap_alloc for situation 3 failed\n");
               return -1;
            }
            all_blocks[block_idx++] = block_lba;

            /* 每分配一个块就将位图同步到硬盘 */
            block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
            bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
         }
         ide_write(cur_part->my_disk, indirect_block_table, all_blocks + 12, 1);   // 同步一级间接块表到硬盘
      } 
   }

   /* 现在写入文件数据所用到的块地址已经收集到all_block中了 */
   bool first_write_block = true;      // 含有剩余空间的扇区标识，如果是第一次写数据，应该将数据中的老数据一同读出来，然后添加新数据后一同同步到硬盘上，这样就保护了老数据
   /* 块地址已经收集到all_blocks中,下面开始写数据 */
   file->fd_pos = file->fd_inode->i_size - 1;   // 置fd_pos为文件大小-1,下面在写数据时随时更新
   while (bytes_written < count) {      // 直到写完所有数据；bytes_written：已经写完了多少字节
      memset(io_buf, 0, BLOCK_SIZE);//io_buf是用来与硬盘“交流”的缓冲区
      sec_idx = file->fd_inode->i_size / BLOCK_SIZE;
      sec_lba = all_blocks[sec_idx];
      sec_off_bytes = file->fd_inode->i_size % BLOCK_SIZE;
      sec_left_bytes = BLOCK_SIZE - sec_off_bytes;

      /* 判断此次写入硬盘的数据大小 */
      chunk_size = size_left < sec_left_bytes ? size_left : sec_left_bytes;
      //size_left初始值为count,是这次调用file_write要写如文件的总字节数
      // chunk_size，是要写入硬盘的字节数
      if (first_write_block) {
         //如果是第一次写入数据，则先将磁盘中的老数据读出来
         ide_read(cur_part->my_disk, sec_lba, io_buf, 1);
         first_write_block = false;
      }
      //将src指向的要写入的源数据拷贝到io_buf中新数据的开始地址处
      memcpy(io_buf + sec_off_bytes, src, chunk_size);
      //同步io_buf的内容到硬盘上
      ide_write(cur_part->my_disk, sec_lba, io_buf, 1);
      // printk("file write at lba 0x%x\n", sec_lba);    //调试用,完成后去掉

      src += chunk_size;   // 将指针推移到下个新数据
      file->fd_inode->i_size += chunk_size;  // 更新文件大小
      file->fd_pos += chunk_size;//更新文件pos   
      bytes_written += chunk_size;
      size_left -= chunk_size;
   }
   inode_sync(cur_part, file->fd_inode, io_buf);
   sys_free(all_blocks);
   sys_free(io_buf);
   return bytes_written;
}

#include "fs.h"
#include "super_block.h"
#include "inode.h"
#include "dir.h"
#include "stdint.h"
#include "stdio-kernel.h"
#include "list.h"
#include "string.h"
#include "ide.h"
#include "global.h"
#include "debug.h"
#include "memory.h"
#include "file.h"

struct partition* cur_part;	 // 默认情况下操作的是哪个分区

/* 挂载分区，实质上把该文件系统的元信息从硬盘上读出来加载到内存中*/
/* 在分区链表中找到名为part_name的分区,并将其指针赋值给cur_part */
static bool mount_partition(struct list_elem* pelem, int arg) {
   char* part_name = (char*)arg;
   struct partition* part = elem2entry(struct partition, part_tag, pelem);
   if (!strcmp(part->name, part_name)) {
      cur_part = part;
      struct disk* hd = cur_part->my_disk;

      /* sb_buf用来存储从硬盘上读入的超级块 */
      struct super_block* sb_buf = (struct super_block*)sys_malloc(SECTOR_SIZE);

      /* 在内存中创建分区cur_part的超级块 */
      cur_part->sb = (struct super_block*)sys_malloc(sizeof(struct super_block));
      if (cur_part->sb == NULL) {
	      PANIC("alloc memory failed!");
      }

      /* 读入超级块 */
      memset(sb_buf, 0, SECTOR_SIZE);
      ide_read(hd, cur_part->start_lba + 1, sb_buf, 1);   

      /* 把sb_buf中超级块的信息复制到分区的超级块sb中。*/
      memcpy(cur_part->sb, sb_buf, sizeof(struct super_block)); 

      /**********     将硬盘上的块位图读入到内存    ****************/
      cur_part->block_bitmap.bits = (uint8_t*)sys_malloc(sb_buf->block_bitmap_sects * SECTOR_SIZE);
      if (cur_part->block_bitmap.bits == NULL) {
         PANIC("alloc memory failed!");
      }
      cur_part->block_bitmap.btmp_bytes_len = sb_buf->block_bitmap_sects * SECTOR_SIZE;
      /* 从硬盘上读入块位图到分区的block_bitmap.bits */
      ide_read(hd, sb_buf->block_bitmap_lba, cur_part->block_bitmap.bits, sb_buf->block_bitmap_sects);   
      /*************************************************************/

      /**********     将硬盘上的inode位图读入到内存    ************/
      cur_part->inode_bitmap.bits = (uint8_t*)sys_malloc(sb_buf->inode_bitmap_sects * SECTOR_SIZE);
      if (cur_part->inode_bitmap.bits == NULL) {
	      PANIC("alloc memory failed!");
      }
      cur_part->inode_bitmap.btmp_bytes_len = sb_buf->inode_bitmap_sects * SECTOR_SIZE;
      /* 从硬盘上读入inode位图到分区的inode_bitmap.bits */
      ide_read(hd, sb_buf->inode_bitmap_lba, cur_part->inode_bitmap.bits, sb_buf->inode_bitmap_sects);   
      /*************************************************************/

      list_init(&cur_part->open_inodes);
      printk("mount %s done!\n", part->name);

   /* 此处返回true是为了迎合主调函数list_traversal的实现,与函数本身功能无关。
      只有返回true时list_traversal才会停止遍历,减少了后面元素无意义的遍历.*/
      return true;
   }
   return false;     // 使list_traversal继续遍历
}

/* 格式化分区,也就是初始化分区的元信息,创建文件系统 */
static void partition_format(struct partition* part) {
/* 为方便实现,一个块大小是一扇区 */
   uint32_t boot_sector_sects = 1;	  
   uint32_t super_block_sects = 1;
   uint32_t inode_bitmap_sects = DIV_ROUND_UP(MAX_FILES_PER_PART, BITS_PER_SECTOR);	   // I结点位图占用的扇区数.最多支持4096个文件
   uint32_t inode_table_sects = DIV_ROUND_UP(((sizeof(struct inode) * MAX_FILES_PER_PART)), SECTOR_SIZE);
   uint32_t used_sects = boot_sector_sects + super_block_sects + inode_bitmap_sects + inode_table_sects;
   uint32_t free_sects = part->sec_cnt - used_sects;  

/************** 简单处理块位图占据的扇区数 ***************/
   uint32_t block_bitmap_sects;
   block_bitmap_sects = DIV_ROUND_UP(free_sects, BITS_PER_SECTOR);
   /* block_bitmap_bit_len是位图中位的长度,也是可用块的数量 */
   uint32_t block_bitmap_bit_len = free_sects - block_bitmap_sects; 
   block_bitmap_sects = DIV_ROUND_UP(block_bitmap_bit_len, BITS_PER_SECTOR); 
/*********************************************************/
   
   /* 超级块初始化 */
   struct super_block sb;
   sb.magic = 0x19590318;
   sb.sec_cnt = part->sec_cnt;
   sb.inode_cnt = MAX_FILES_PER_PART;
   sb.part_lba_base = part->start_lba;

   sb.block_bitmap_lba = sb.part_lba_base + 2;	 // 第0块是引导块,第1块是超级块
   sb.block_bitmap_sects = block_bitmap_sects;

   sb.inode_bitmap_lba = sb.block_bitmap_lba + sb.block_bitmap_sects;
   sb.inode_bitmap_sects = inode_bitmap_sects;

   sb.inode_table_lba = sb.inode_bitmap_lba + sb.inode_bitmap_sects;
   sb.inode_table_sects = inode_table_sects; 

   sb.data_start_lba = sb.inode_table_lba + sb.inode_table_sects;
   sb.root_inode_no = 0;
   sb.dir_entry_size = sizeof(struct dir_entry);

   printk("%s info:\n", part->name);
   printk("   magic:0x%x\n   part_lba_base:0x%x\n   all_sectors:0x%x\n   inode_cnt:0x%x\n   block_bitmap_lba:0x%x\n   block_bitmap_sectors:0x%x\n   inode_bitmap_lba:0x%x\n   inode_bitmap_sectors:0x%x\n   inode_table_lba:0x%x\n   inode_table_sectors:0x%x\n   data_start_lba:0x%x\n", sb.magic, sb.part_lba_base, sb.sec_cnt, sb.inode_cnt, sb.block_bitmap_lba, sb.block_bitmap_sects, sb.inode_bitmap_lba, sb.inode_bitmap_sects, sb.inode_table_lba, sb.inode_table_sects, sb.data_start_lba);

   struct disk* hd = part->my_disk;
/*******************************
 * 1 将超级块写入本分区的1扇区 *
 ******************************/
   ide_write(hd, part->start_lba + 1, &sb, 1);
   printk("   super_block_lba:0x%x\n", part->start_lba + 1);

/* 找出数据量最大的元信息,用其尺寸做存储缓冲区*/
   uint32_t buf_size = (sb.block_bitmap_sects >= sb.inode_bitmap_sects ? sb.block_bitmap_sects : sb.inode_bitmap_sects);
   buf_size = (buf_size >= sb.inode_table_sects ? buf_size : sb.inode_table_sects) * SECTOR_SIZE;
   uint8_t* buf = (uint8_t*)sys_malloc(buf_size);	// 申请的内存由内存管理系统清0后返回
   
/**************************************
 * 2 将块位图初始化并写入sb.block_bitmap_lba *
 *************************************/
   /* 初始化块位图block_bitmap */
   buf[0] |= 0x01;       // 第0个块预留给根目录,位图中先占位
   uint32_t block_bitmap_last_byte = block_bitmap_bit_len / 8;
   uint8_t  block_bitmap_last_bit  = block_bitmap_bit_len % 8;
   uint32_t last_size = SECTOR_SIZE - (block_bitmap_last_byte % SECTOR_SIZE);	     // last_size是位图所在最后一个扇区中，不足一扇区的其余部分

   /* 1 先将位图最后一字节到其所在的扇区的结束全置为1,即超出实际块数的部分直接置为已占用*/
   memset(&buf[block_bitmap_last_byte], 0xff, last_size);
   
   /* 2 再将上一步中覆盖的最后一字节内的有效位重新置0 */
   uint8_t bit_idx = 0;
   while (bit_idx <= block_bitmap_last_bit) {
      buf[block_bitmap_last_byte] &= ~(1 << bit_idx++);
   }
   ide_write(hd, sb.block_bitmap_lba, buf, sb.block_bitmap_sects);

/***************************************
 * 3 将inode位图初始化并写入sb.inode_bitmap_lba *
 ***************************************/
   /* 先清空缓冲区*/
   memset(buf, 0, buf_size);
   buf[0] |= 0x1;      // 第0个inode分给了根目录
   /* 由于inode_table中共4096个inode,位图inode_bitmap正好占用1扇区,
    * 即inode_bitmap_sects等于1, 所以位图中的位全都代表inode_table中的inode,
    * 无须再像block_bitmap那样单独处理最后一扇区的剩余部分,
    * inode_bitmap所在的扇区中没有多余的无效位 */
   ide_write(hd, sb.inode_bitmap_lba, buf, sb.inode_bitmap_sects);

/***************************************
 * 4 将inode数组初始化并写入sb.inode_table_lba *
 ***************************************/
 /* 准备写inode_table中的第0项,即根目录所在的inode */
   memset(buf, 0, buf_size);  // 先清空缓冲区buf
   struct inode* i = (struct inode*)buf; 
   i->i_size = sb.dir_entry_size * 2;	 // .和..
   i->i_no = 0;   // 根目录占inode数组中第0个inode
   i->i_sectors[0] = sb.data_start_lba;	     // 由于上面的memset,i_sectors数组的其它元素都初始化为0 
   ide_write(hd, sb.inode_table_lba, buf, sb.inode_table_sects);
   
/***************************************
 * 5 将根目录初始化并写入sb.data_start_lba
 ***************************************/
   /* 写入根目录的两个目录项.和.. */
   memset(buf, 0, buf_size);
   struct dir_entry* p_de = (struct dir_entry*)buf;

   /* 初始化当前目录"." */
   memcpy(p_de->filename, ".", 1);
   p_de->i_no = 0;
   p_de->f_type = FT_DIRECTORY;
   p_de++;

   /* 初始化当前目录父目录".." */
   memcpy(p_de->filename, "..", 2);
   p_de->i_no = 0;   // 根目录的父目录依然是根目录自己
   p_de->f_type = FT_DIRECTORY;

   /* sb.data_start_lba已经分配给了根目录,里面是根目录的目录项 */
   ide_write(hd, sb.data_start_lba, buf, 1);

   printk("   root_dir_lba:0x%x\n", sb.data_start_lba);
   printk("%s format done\n", part->name);
   sys_free(buf);
}

/* 将最上层路径名称解析出来 
   ----------------------------------
   参数 ：
   pathname : 需要解析的路径名，也作为返回值返回
   name_store: pathname中最顶层的路径,是一个传出参数，由调用者提供
   返回值：
   pathname， 它是参数也是返回值
   ------------------------------------
   比如：
       pathname = "/a/b/c"
       那么调用函数后，name_store = "a", pathname = "/b/c"
*/
static char* path_parse(char* pathname, char* name_store) {
   if (pathname[0] == '/') {   // 根目录不需要单独解析
    /* 路径中出现1个或多个连续的字符'/',将这些'/'跳过,如"///a/b" */
       while(*(++pathname) == '/');
   }

   /* 开始一般的路径解析 */
   while (*pathname != '/' && *pathname != 0) {
      *name_store++ = *pathname++;
   }

   if (pathname[0] == 0) {   // 若路径字符串为空则返回NULL
      return NULL;
   }
   return pathname; 
}

/* 返回路径深度,比如/a/b/c,深度为3，输入"/a//b///c"，还是返回3
 */
int32_t path_depth_cnt(char* pathname) {
   ASSERT(pathname != NULL);
   char* p = pathname;
   char name[MAX_FILE_NAME_LEN];       // 用于path_parse的参数做路径解析
   uint32_t depth = 0;

   /* 解析路径,从中拆分出各级名称 */ 
   p = path_parse(p, name);
   while (name[0]) {
      depth++;
      memset(name, 0, MAX_FILE_NAME_LEN);
      if (p) {	     // 如果p不等于NULL,继续分析路径
	      p  = path_parse(p, name);
      }
   }
   return depth;
}


/* 搜索文件pathname,若找到则返回其inode号,否则返回-1 
*-------------------------------------------------------- 
* 参数 ：
*      patname : 被检索的文件
*      path_search_record ： 传出参数，用来记录查找文件过程中已找到的上级路径,也就是查找文件过程中"走过的地方；
*----------------------------------------------------------
*如果找到了某个或者文件，就返回对应的inode节点号，而且path_search_record结构中的parent_dir记录了查找文件的父目录
*     比如， 存在"/a/b/c" ， 那么path_search_record结构中的parent_dir就是b目录
*如果没有则返回-1，如果主调函数想要创建这个文件或者目录，那么它得知道在哪里创建，
*  在path_search_record结构中的parent_dir就记录了这个信息
*/
static int search_file(const char* pathname, struct path_search_record* searched_record) {
   /* 如果待查找的是根目录,为避免下面无用的查找,直接返回已知根目录信息 */
   if (!strcmp(pathname, "/") || !strcmp(pathname, "/.") || !strcmp(pathname, "/..")) {
      searched_record->parent_dir = &root_dir;
      searched_record->file_type = FT_DIRECTORY;
      searched_record->searched_path[0] = 0;	   // 搜索路径置空
      return 0;
   }

   uint32_t path_len = strlen(pathname);
   /* 保证pathname至少是这样的路径/x且小于最大长度 */
   ASSERT(pathname[0] == '/' && path_len > 1 && path_len < MAX_PATH_LEN);
   char* sub_path = (char*)pathname;
   struct dir* parent_dir = &root_dir;	//根目录
   struct dir_entry dir_e;

   /* 记录路径解析出来的各级名称,如路径"/a/b/c",
    * 数组name每次的值分别是"a","b","c" */
   char name[MAX_FILE_NAME_LEN] = {0};

   searched_record->parent_dir = parent_dir;
   searched_record->file_type = FT_UNKNOWN;
   uint32_t parent_inode_no = 0;  // 父目录的inode号
   
   sub_path = path_parse(sub_path, name);
   while (name[0]) {	   // 若第一个字符就是结束符,结束循环
      /* 记录查找过的路径,但不能超过searched_path的长度512字节 */
      ASSERT(strlen(searched_record->searched_path) < 512);

      /* 记录已存在的父目录 */
      strcat(searched_record->searched_path, "/");
      strcat(searched_record->searched_path, name);

      /* 在所给的目录中查找文件 */
      if (search_dir_entry(cur_part, parent_dir, name, &dir_e)) {
         memset(name, 0, MAX_FILE_NAME_LEN);
         /* 若sub_path不等于NULL,也就是未结束时继续拆分路径 */
         if (sub_path) {
            sub_path = path_parse(sub_path, name);
         }

         if (FT_DIRECTORY == dir_e.f_type) {   // 如果被打开的是目录
            parent_inode_no = parent_dir->inode->i_no;//备份父目录inode编号，会在最后一级路径为目录的情况下用到
            dir_close(parent_dir); //关闭原来的父目录
            parent_dir = dir_open(cur_part, dir_e.i_no); // 打开新的父目录，并赋值给parent_dir
            searched_record->parent_dir = parent_dir; //记录searched_record->parent_dir的父目录
            continue;
         } else if (FT_REGULAR == dir_e.f_type) {	 // 若是普通文件
            searched_record->file_type = FT_REGULAR;
            return dir_e.i_no;//直接返回，search_file函数不负责处理错误，交给上层函数查看searched_record结构体中的记录判断返回什么错误
            /* 
               比如 如果搜索 /a/c， 但是根目录下没有a目录，只有a文件，按理来说我们要返回一个错误，但是search_file函数不负责处理错误，所以这里直接返回
               信息已经全部记录在searched_record中了，有主调函数判断正误
             */
         }
      } else {		   //若找不到,则返回-1
         /* 找不到目录项时,要留着parent_dir不要关闭,
         * 若是创建新文件的话需要在parent_dir中创建 */
         return -1;
      }
   }

   /* 执行到此,必然是遍历了完整路径并且查找的文件或目录只有同名目录存在 */
   dir_close(searched_record->parent_dir);	      

   /* 保存被查找目录的直接父目录 */
   searched_record->parent_dir = dir_open(cur_part, parent_inode_no);	   
   searched_record->file_type = FT_DIRECTORY;
   return dir_e.i_no;
}


/* 打开或创建文件成功后,返回文件描述符，即pcb中fdtable的下标,否则返回-1 
-------------------------------------------------------
flag = O_CREATE | O_WRONLU | O_TRUNC 时，创建文件
---------------------------------------------------------
只支持文件打开，而不支持目录打开，所以如果path = "/a/"则返回-1， 如果path ="/a" , 则执行正常处理流程

*/
int32_t sys_open(const char* pathname, uint8_t flags) {
  /* 对目录要用dir_open,这里只有open文件 */
   if (pathname[strlen(pathname) - 1] == '/') {//判断路径为目录
      printk("can`t open a directory %s\n",pathname);
      return -1;
   }
   ASSERT(flags <= 7);
   int32_t fd = -1;	   // 默认为找不到

   struct path_search_record searched_record;
   memset(&searched_record, 0, sizeof(struct path_search_record));

   /* 记录目录深度.帮助判断中间某个目录不存在的情况 */
   uint32_t pathname_depth = path_depth_cnt((char*)pathname);

   /* 先检查文件是否存在 */
   int inode_no = search_file(pathname, &searched_record);
   bool found = inode_no != -1 ? true : false; 

   if (searched_record.file_type == FT_DIRECTORY) {
      printk("can`t open a direcotry with open(), use opendir() to instead\n");
      dir_close(searched_record.parent_dir);
      return -1;
   }

   uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);

   /* 先判断是否把pathname的各层目录都访问到了,即是否在某个中间目录就失败了 */
   if (pathname_depth != path_searched_depth) {   // 说明并没有访问到全部的路径,某个中间目录是不存在的
      printk("cannot access %s: Not a directory, subpath %s is`t exist\n", \
	    pathname, searched_record.searched_path);
      dir_close(searched_record.parent_dir);
      return -1;
   }

   /* 若是在最后一个路径上没找到,并且并不是要创建文件,直接返回-1 */
   if (!found && !(flags & O_CREAT)) {
      printk("in path %s, file %s is`t exist\n", \
	    searched_record.searched_path, \
	    (strrchr(searched_record.searched_path, '/') + 1));
      dir_close(searched_record.parent_dir);
      return -1;
   } else if (found && flags & O_CREAT) {  // 若要创建的文件已存在
      printk("%s has already exist!\n", pathname);
      dir_close(searched_record.parent_dir);
      return -1;
   }
  //能够执行到这里标识，没找到文件，并且flag为创建文件

   switch (flags & O_CREAT) {
      case O_CREAT:
         printk("creating file\n");
         fd = file_create(searched_record.parent_dir, (strrchr(pathname, '/') + 1), flags);
         dir_close(searched_record.parent_dir);//关闭父目录
         // 其余为打开文件的情况
      default:
         fd = file_open(inode_no,flags);
   }

   /* 此fd是指任务pcb->fd_table数组中的元素下标,
    * 并不是指全局file_table中的下标 */
   return fd;
}

/* 将文件描述符转化为文件表的下标 */
static uint32_t fd_local2global(uint32_t local_fd) {
   struct task_struct* cur = running_thread();
   int32_t global_fd = cur->fd_table[local_fd];  
   ASSERT(global_fd >= 0 && global_fd < MAX_FILE_OPEN);
   return (uint32_t)global_fd;
} 

/* 关闭文件描述符fd指向的文件,成功返回0,否则返回-1 
--------------------------------------------------
原理： 首先调用fd_local2global将进程局部描述符转化为全局描述符，然后调用file_close对全局描述符做一次关闭操作
*/
int32_t sys_close(int32_t fd) {
   int32_t ret = -1;   // 返回值默认为-1,即失败
   if (fd > 2) {
      uint32_t _fd = fd_local2global(fd);
      ret = file_close(&file_table[_fd]);
      running_thread()->fd_table[fd] = -1; // 使该文件描述符位可用
   }
   return ret;
}


/* 将buf中连续count个字节写入文件描述符fd,成功则返回写入的字节数,失败返回-1 
-----------------------------------------------------------------------------
原理 ：
     1.如果fd == stdout_no，那么先将buf中的数据复制到临时缓冲区，然后再调用console_put_str将临时缓冲区中的数据写到标准输出即可
     2.如果fd 不是标准输出， 先将进程局部描述符转换成全局描述符，使用这个全局描述符在文件表中索引到对应file结构，然后调用file.c/file_write即可
     3.当然在第二种情况中，需要检查文件flag是否满足写模式，否则返回-1，并在中断上输出错误信息
*/
int32_t sys_write(int32_t fd, const void* buf, uint32_t count) {
   if (fd < 0) {
      printk("sys_write: fd error\n");
      return -1;
   }
   if (fd == stdout_no) {  
      char tmp_buf[1024] = {0};
      memcpy(tmp_buf, buf, count);
      console_put_str(tmp_buf);
      return count;
   }
   uint32_t _fd = fd_local2global(fd);
   struct file* wr_file = &file_table[_fd];
   if (wr_file->fd_flag & O_WRONLY || wr_file->fd_flag & O_RDWR) {
      uint32_t bytes_written  = file_write(wr_file, buf, count);
      return bytes_written;
   } else {
      console_put_str("sys_write: not allowed to write file without flag O_RDWR or O_WRONLY\n");
      return -1;
   }
}

/* 从文件描述符fd指向的文件中读取count个字节到buf,若成功则返回读出的字节数,到文件尾则返回-1 
---------------------------------------------------------------------------------------
原理：
      1.将进程局部描述符转化为全局文件描述符 _fd
      2.调用file_read( &file_tabel[_fd],buf,count ) 即可
*/
int32_t sys_read(int32_t fd, void* buf, uint32_t count) {
   if (fd < 0) {
      printk("sys_read: fd error\n");
      return -1;
   }
   ASSERT(buf != NULL);
   uint32_t _fd = fd_local2global(fd);
   return file_read(&file_table[_fd], buf, count);   
}


/* 重置用于文件读写操作的偏移指针,成功时返回新的偏移量,出错时返回-1 
-------------------------------------------------------------------
注意： 指针相对于文件头的偏移量不能超过文件的大小！！
*/
int32_t sys_lseek(int32_t fd, int32_t offset, uint8_t whence) {
   if (fd < 0) {
      printk("sys_lseek: fd error\n");
      return -1;
   }
   ASSERT(whence > 0 && whence < 4);
   uint32_t _fd = fd_local2global(fd);
   struct file* pf = &file_table[_fd];
   int32_t new_pos = 0;   //新的偏移量必须位于文件大小之内
   int32_t file_size = (int32_t)pf->fd_inode->i_size;
   switch (whence) {
      /* SEEK_SET 新的读写位置是相对于文件开头再增加offset个位移量 */
      case SEEK_SET:
         new_pos = offset;
         break;

      /* SEEK_CUR 新的读写位置是相对于当前的位置增加offset个位移量 */
      case SEEK_CUR:	// offse可正可负
         new_pos = (int32_t)pf->fd_pos + offset;
         break;

      /* SEEK_END 新的读写位置是相对于文件尺寸再增加offset个位移量 */
      case SEEK_END:	   // 此情况下,offset应该为负值
         new_pos = file_size + offset;
   }
   if (new_pos < 0 || new_pos > (file_size - 1)) {	 
      //新的偏移量不能超过文件大小
      return -1;
   }
   pf->fd_pos = new_pos;
   return pf->fd_pos;
}

/* 删除文件( --非目录-- ),成功返回0,失败返回-1 */
int32_t sys_unlink(const char* pathname) {
   ASSERT(strlen(pathname) < MAX_PATH_LEN);

   /* 先检查待删除的文件是否存在 */
   struct path_search_record searched_record;
   memset(&searched_record, 0, sizeof(struct path_search_record));
   int inode_no = search_file(pathname, &searched_record);
   ASSERT(inode_no != 0);
   if (inode_no == -1) {
      printk("file %s not found!\n", pathname);
      dir_close(searched_record.parent_dir);//没找到对应文件，删除searched_record中最近打开的父目录，防止浪费内存
      return -1;
   }
   if (searched_record.file_type == FT_DIRECTORY) {
      //给的路径是一个目录
      printk("can`t delete a direcotry with unlink(), use rmdir() to instead\n");
      dir_close(searched_record.parent_dir);
      return -1;
   }

   /* 检查是否在已打开文件列表(文件表)中 */
   uint32_t file_idx = 0;
   while (file_idx < MAX_FILE_OPEN) {
      if (file_table[file_idx].fd_inode != NULL && (uint32_t)inode_no == file_table[file_idx].fd_inode->i_no) {
         //要删除的文件在内存中
	      break;
      }
      file_idx++;
   }
   if (file_idx < MAX_FILE_OPEN) {
      //要删除的文件在内存中，说明有其他进程使用这个文件，不能删除
      dir_close(searched_record.parent_dir);
      printk("file %s is in use, not allow to delete!\n", pathname);
      return -1;
   }
   ASSERT(file_idx == MAX_FILE_OPEN);
   
   /* 为delete_dir_entry申请缓冲区 */
   void* io_buf = sys_malloc(SECTOR_SIZE + SECTOR_SIZE);
   if (io_buf == NULL) {
      dir_close(searched_record.parent_dir);
      printk("sys_unlink: malloc for io_buf failed\n");
      return -1;
   }

   struct dir* parent_dir = searched_record.parent_dir;  
   delete_dir_entry(cur_part, parent_dir, inode_no, io_buf);//释放文件在硬盘中的direntry，跟新父目录inode
   inode_release(cur_part, inode_no);//将目录项本身的inode删除
   sys_free(io_buf);
   dir_close(searched_record.parent_dir);//关闭查找过程中打开的父目录
   return 0;   // 成功删除文件 
}

/* 创建目录pathname,成功返回0,失败返回-1 */
int32_t sys_mkdir(const char* pathname) {
   uint8_t rollback_step = 0;	       // 用于操作失败时回滚各资源状态
   void* io_buf = sys_malloc(SECTOR_SIZE * 2);
   if (io_buf == NULL) {
      printk("sys_mkdir: sys_malloc for io_buf failed\n");
      return -1;
   }

   struct path_search_record searched_record;
   memset(&searched_record, 0, sizeof(struct path_search_record));
   int inode_no = -1;
   inode_no = search_file(pathname, &searched_record);
   if (inode_no != -1) {      // 找到了同名目录或文件,目录已经存在，失败返回
      printk("sys_mkdir: file or directory %s exist!\n", pathname);
      rollback_step = 1;
      goto rollback;
   } else {	     // 若未找到,也要判断是在最终目录没找到还是某个中间目录不存在
      uint32_t pathname_depth = path_depth_cnt((char*)pathname);//计算输入参数pathname中的路径深度
      uint32_t path_searched_depth = path_depth_cnt(searched_record.searched_path);//计算搜索路径中记录的路径深度
      /* 先判断是否把pathname的各层目录都访问到了,即是否在某个中间目录就失败了 */
      if (pathname_depth != path_searched_depth) {   // 说明并没有访问到全部的路径,某个中间目录是不存在的
         printk("sys_mkdir: can`t access %s, subpath %s is`t exist\n", pathname, searched_record.searched_path);
         rollback_step = 1;
         goto rollback;
      }
   }

   struct dir* parent_dir = searched_record.parent_dir;
   /* 目录名称后可能会有字符'/',所以最好直接用searched_record.searched_path,无'/' */
   /*  strrchr: 从后往前（注意从后往前），查找字符串str中首次出现字符ch的地址*/
   char* dirname = strrchr(searched_record.searched_path, '/') + 1;

   inode_no = inode_bitmap_alloc(cur_part); 
   if (inode_no == -1) {
      printk("sys_mkdir: allocate inode failed\n");
      rollback_step = 1;
      goto rollback;
   }

   struct inode new_dir_inode;
   inode_init(inode_no, &new_dir_inode);	    // 初始化i结点

   uint32_t block_bitmap_idx = 0;     // 用来记录block对应于block_bitmap中的索引
   int32_t block_lba = -1;
/* 为目录分配一个块,用来写入目录.和.. */
   block_lba = block_bitmap_alloc(cur_part);
   if (block_lba == -1) {
      printk("sys_mkdir: block_bitmap_alloc for create directory failed\n");
      rollback_step = 2;
      goto rollback;
   }
   new_dir_inode.i_sectors[0] = block_lba;
   /* 每分配一个块就将位图同步到硬盘 */
   block_bitmap_idx = block_lba - cur_part->sb->data_start_lba;
   ASSERT(block_bitmap_idx != 0);
   bitmap_sync(cur_part, block_bitmap_idx, BLOCK_BITMAP);
   
   /* 将当前目录的目录项'.'和'..'写入目录 */
   memset(io_buf, 0, SECTOR_SIZE * 2);	 // 清空io_buf
   struct dir_entry* p_de = (struct dir_entry*)io_buf;
   
   /* 初始化当前目录"." */
   memcpy(p_de->filename, ".", 1);
   p_de->i_no = inode_no ;
   p_de->f_type = FT_DIRECTORY;

   p_de++;
   /* 初始化当前目录".." */
   memcpy(p_de->filename, "..", 2);
   p_de->i_no = parent_dir->inode->i_no;
   p_de->f_type = FT_DIRECTORY;
   ide_write(cur_part->my_disk, new_dir_inode.i_sectors[0], io_buf, 1);

   new_dir_inode.i_size = 2 * cur_part->sb->dir_entry_size;

   /* 在父目录中添加自己的目录项 */
   struct dir_entry new_dir_entry;
   memset(&new_dir_entry, 0, sizeof(struct dir_entry));
   
   create_dir_entry(dirname, inode_no, FT_DIRECTORY, &new_dir_entry);
   memset(io_buf, 0, SECTOR_SIZE * 2);	 // 清空io_buf
   if (!sync_dir_entry(parent_dir, &new_dir_entry, io_buf)) {	  // sync_dir_entry中将block_bitmap通过bitmap_sync同步到硬盘
      printk("sys_mkdir: sync_dir_entry to disk failed!\n");
      rollback_step = 2;
      goto rollback;
   }

   /* 父目录的inode同步到硬盘 */
   memset(io_buf, 0, SECTOR_SIZE * 2);
   inode_sync(cur_part, parent_dir->inode, io_buf);

   /* 将新创建目录的inode同步到硬盘 */
   memset(io_buf, 0, SECTOR_SIZE * 2);
   inode_sync(cur_part, &new_dir_inode, io_buf);

   /* 将inode位图同步到硬盘 */
   bitmap_sync(cur_part, inode_no, INODE_BITMAP);

   sys_free(io_buf);

   /* 关闭所创建目录的父目录 */
   dir_close(searched_record.parent_dir);
   return 0;

/*创建文件或目录需要创建相关的多个资源,若某步失败则会执行到下面的回滚步骤 */
rollback:	     // 因为某步骤操作失败而回滚
   switch (rollback_step) {
      case 2:
	 bitmap_set(&cur_part->inode_bitmap, inode_no, 0);	 // 如果新文件的inode创建失败,之前位图中分配的inode_no也要恢复 
      case 1:
	 /* 关闭所创建目录的父目录 */
	 dir_close(searched_record.parent_dir);
	 break;
   }
   sys_free(io_buf);
   return -1;
}

/* 在磁盘上搜索文件系统,若没有则格式化分区创建文件系统
-----------------------------------------------------
1.设置默认分区，调用mount_partition将该分区文件系统的元信息从硬盘上读出来加载到内存中，存储在struct partition cur_part这个数据结构中
2.将当前分区的根目录打开
3. 初始化全局文件表

 */
void filesys_init() {
   uint8_t channel_no = 0, dev_no, part_idx = 0;

   /* sb_buf用来存储从硬盘上读入的超级块 */
   struct super_block* sb_buf = (struct super_block*)sys_malloc(SECTOR_SIZE);

   if (sb_buf == NULL) {
      PANIC("alloc memory failed!");
   }
   printk("searching filesystem......\n");
   while (channel_no < channel_cnt) {
      dev_no = 0;
      while(dev_no < 2) {
	 if (dev_no == 0) {   // 跨过裸盘hd60M.img
	    dev_no++;
	    continue;
	 }
	 struct disk* hd = &channels[channel_no].devices[dev_no];
	 struct partition* part = hd->prim_parts;
	 while(part_idx < 12) {   // 4个主分区+8个逻辑
	    if (part_idx == 4) {  // 开始处理逻辑分区
	       part = hd->logic_parts;
	    }
	 
	 /* channels数组是全局变量,默认值为0,disk属于其嵌套结构,
	  * partition又为disk的嵌套结构,因此partition中的成员默认也为0.
	  * 若partition未初始化,则partition中的成员仍为0. 
	  * 下面处理存在的分区. */
	    if (part->sec_cnt != 0) {  // 如果分区存在
	       memset(sb_buf, 0, SECTOR_SIZE);

	       /* 读出分区的超级块,根据魔数是否正确来判断是否存在文件系统 */
	       ide_read(hd, part->start_lba + 1, sb_buf, 1);   

	       /* 只支持自己的文件系统.若磁盘上已经有文件系统就不再格式化了 */
	       if (sb_buf->magic == 0x19590318) {
            printk("%s has filesystem\n", part->name);
	       } else {			  // 其它文件系统不支持,一律按无文件系统处理
            printk("formatting %s`s partition %s......\n", hd->name, part->name);
            partition_format(part);
	       }
	    }
	    part_idx++;
	    part++;	// 下一分区
	 }
	 dev_no++;	// 下一磁盘
      }
      channel_no++;	// 下一通道
   }
   sys_free(sb_buf);

   /* 确定默认操作的分区 */
   char default_part[8] = "sdb1";
   /* 挂载分区 ，默认为sdb1分区*/
   list_traversal(&partition_list, mount_partition, (int)default_part);
   printk("%s info:\n", cur_part->name);
   struct super_block* sb = cur_part->sb;
   printk("   magic:0x%x\n   part_lba_base:0x%x\n   all_sectors:0x%x\n   inode_cnt:0x%x\n   block_bitmap_lba:0x%x\n   block_bitmap_sectors:0x%x\n   inode_bitmap_lba:0x%x\n   inode_bitmap_sectors:0x%x\n   inode_table_lba:0x%x\n   inode_table_sectors:0x%x\n   data_start_lba:0x%x\n", sb->magic, sb->part_lba_base, sb->sec_cnt, sb->inode_cnt, sb->block_bitmap_lba, sb->block_bitmap_sects, sb->inode_bitmap_lba, sb->inode_bitmap_sects, sb->inode_table_lba, sb->inode_table_sects, sb->data_start_lba);

   /* 将当前分区的根目录打开 */
   open_root_dir(cur_part);

   /* 初始化文件表 */
   uint32_t fd_idx = 0;
   while (fd_idx < MAX_FILE_OPEN) {
      file_table[fd_idx++].fd_inode = NULL;
   }
}

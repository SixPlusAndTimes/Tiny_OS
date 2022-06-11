#ifndef __FS_INODE_H
#define __FS_INODE_H
#include "stdint.h"
#include "list.h"

// inode数据结构
struct inode {
   uint32_t i_no;    // inode编号

/* 当此inode是文件时,i_size是指文件大小，单位字节,
若此inode是目录,i_size是指该目录下所有目录项大小之和*/
   uint32_t i_size;

   uint32_t i_open_cnts;   // 记录此文件被打开的次数，在关闭文件时，回收与之相关的资源时使用
   bool write_deny;	   // 写文件不能并行,进程写文件前检查此标识。当此标志位true时，表示已经有任务在写这个文件里， 因此此文件的其他写操作应该被拒绝

/* i_sectors[0-11]是直接块, i_sectors[12]用来存储一级间接块指针 */
//i_sectors[12]指向另一个512字节的扇区，且每一个块地址用4字节表示，因此总给那个支持12 + 128 =140 个块（扇区）
   uint32_t i_sectors[13];
   struct list_elem inode_tag;//当一个文件被打开时要将对应的inode加载到内存时，将这个标签加入内存的缓冲队列，如果某个进程再次打开这个文件，那么先在缓冲队列中查找相关的inode，否则再从磁盘上加载啊inod
};
#endif

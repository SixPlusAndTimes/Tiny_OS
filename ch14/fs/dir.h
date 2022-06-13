#ifndef __FS_DIR_H
#define __FS_DIR_H
#include "stdint.h"
#include "inode.h"
#include "ide.h"
#include "global.h"

#define MAX_FILE_NAME_LEN  16	 // 最大文件名长度

/* 目录结构 */
struct dir {//注意 ！ 本结构体存在于内存中，而不是硬盘上
   struct inode* inode;   
   uint32_t dir_pos;	  // 记录在目录内的偏移，值应该位目录项大小的整数倍，这与遍历目录的操作有关
   uint8_t dir_buf[512];  // 目录的数据缓存
};

/* 目录项结构 */
struct dir_entry {//是连接文件名与inode的纽带，本结构体一开始存在于硬盘上，能被加载进内存中
   char filename[MAX_FILE_NAME_LEN];  // 普通文件或目录名称
   uint32_t i_no;		      // 普通文件或目录对应的inode编号
   enum file_types f_type;	      // 文件类型
};

#endif

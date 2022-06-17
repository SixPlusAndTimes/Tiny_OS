#ifndef __FS_DIR_H
#define __FS_DIR_H
#include "stdint.h"
#include "inode.h"
#include "ide.h"
#include "global.h"

#define MAX_FILE_NAME_LEN  16	 // 最大文件名长度

/* 目录结构 
注意 ！ 本结构体存在于内存中，而不是硬盘上
----------
 struct inode* inode;   //本目录对应的inode指针
   uint32_t dir_pos;	  // 记录在目录内的偏移，值应该位目录项大小的整数倍，这与遍历目录的操作有关
   uint8_t dir_buf[512];  // 目录的数据缓存
*/
struct dir {
   struct inode* inode;   //本目录对应的inode指针
   uint32_t dir_pos;	  // 记录在目录内的偏移，值应该位目录项大小的整数倍，这与遍历目录的操作有关
   uint8_t dir_buf[512];  // 目录的数据缓存
};

/* 目录项结构 */
struct dir_entry {//是连接文件名与inode的纽带，本结构体一开始存在于硬盘上，能被加载进内存中
   char filename[MAX_FILE_NAME_LEN];  // 普通文件或目录名称
   uint32_t i_no;		      // 普通文件或目录对应的inode编号
   enum file_types f_type;	      // 文件类型
};

extern struct dir root_dir;             // 根目录
void open_root_dir(struct partition* part);
struct dir* dir_open(struct partition* part, uint32_t inode_no);
void dir_close(struct dir* dir);
bool search_dir_entry(struct partition* part, struct dir* pdir, const char* name, struct dir_entry* dir_e);
void create_dir_entry(char* filename, uint32_t inode_no, uint8_t file_type, struct dir_entry* p_de);
bool sync_dir_entry(struct dir* parent_dir, struct dir_entry* p_de, void* io_buf);
#endif

#endif

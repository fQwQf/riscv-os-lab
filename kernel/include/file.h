/* file.h */
#ifndef FILE_H
#define FILE_H

#include "types.h"
#include "fs.h"

/* ========== file type（文件描述符类型）========== */
#define FD_NONE 0 // 未使用
#define FD_INODE 1 // inode 文件（本实验只支持这一种）

struct inode {
  uint dev;  /* 设备号 */
  uint inum; /* inode 编号 */
  int ref;   /* 引用计数 */
  int valid; /* 内容是否从磁盘读入 */
  /* 以下字段来自磁盘 dinode */
  short type;
  short major;
  short minor;
  short nlink;
  uint size;
  uint addrs[NDIRECT + 1];
};

struct file {
  int type; // FD_NONE=0, FD_INODE=1
  int ref;
  char readable;
  char writable;
  struct inode *ip;
  uint off;
};

#endif

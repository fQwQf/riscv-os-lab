#ifndef FS_H
#define FS_H

#include "types.h"

#define BSIZE 1024                       /* 磁盘块大小（字节）*/
#define NDIRECT 12                       /* 直接块指针数量 */
#define NINDIRECT (BSIZE / sizeof(uint)) /* 一级间接块中的指针数量 */
#define DIRSIZ 14                        /* 目录项中文件名的最大长度 */

/* ========== 文件系统魔数 ========== */
#define FSMAGIC 0x10203040 // xv6 文件系统魔数

/* ========== 根目录 inode 编号 ========== */
#define ROOTINO 1 // 根目录的 inode 编号（固定为 1）

/* ========== inode type（文件类型）========== */
#define T_FILE 1 // 普通文件
#define T_DIR 2 // 目录文件
#define T_DEVICE 3 // 设备文件

struct superblock {
  uint magic;
  uint size;
  uint nblocks;
  uint ninodes;
  uint nlog;
  uint logstart;
  uint inodestart;
  uint bmapstart;
};

struct dinode {
  short type;  /* 文件类型（0=空闲, 1=普通文件, 2=目录, 3=设备）*/
  short major; /* 设备主号（仅 type==3 时有效）*/
  short minor; /* 设备次号（仅 type==3 时有效）*/
  short nlink; /* 硬链接计数 */
  uint size;   /* 文件大小（字节数）*/
  uint addrs[NDIRECT + 1]; /* 数据块地址 */
};

struct dirent {
  ushort inum;       /* 该条目对应的 inode 编号（0 表示空洞/已删除）*/
  char name[DIRSIZ]; /* 文件名 */
};

#endif

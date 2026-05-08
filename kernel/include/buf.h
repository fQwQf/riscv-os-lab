#ifndef BUF_H
#define BUF_H

#include "types.h"

#define BSIZE 1024

struct buf {
  int valid;         /* 当前缓存的数据是否有效（从磁盘读取过）*/
  int disk;          /* 是否正在与磁盘驱动交互（等待读写完成）*/
  uint dev;          /* 设备号 */
  uint blockno;      /* 磁盘块号 */
  uint refcnt;       /* 引用计数（有多少人在使用这块缓冲）*/
  struct buf *prev;  /* LRU 链表前驱 */
  struct buf *next;  /* LRU 链表后继 */
  uchar data[BSIZE]; /* 磁盘块的实际数据（1024字节）*/
};

#endif

/* mkfs.c — 创建 xv6 文件系统镜像
 *
 * 在宿主机（Linux）上用普通 gcc 编译运行，生成 fs.img。
 * 布局：超级块 | inode 区 | 位图 | 数据区
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <stdint.h>

/* 从内核头文件引入磁盘上的数据结构 */
#include "types.h"
#include "fs.h"

/* FS 布局参数 */
#define FSSIZE     1000   /* 文件系统总块数 */
#define NINODEBLOCKS 2    /* inode 区占用的块数 */

/* 超级块位置 */
#define SBLOCK 1

/* 布局（1个超级块 + inode区 + 位图区 + 数据区）*/
static int nbitmap;     /* 位图块数 */
static int nmeta;       /* 元数据块总数 */
static int nblocks;     /* 数据块总数 */
static int ninodes;     /* 最大 inode 数 */

static int fsfd;         /* 镜像文件描述符 */
static struct superblock sb;

static void wsect(uint sec, void *buf);
static void rsect(uint sec, void *buf);
static uint ialloc(ushort type);
static void iappend(uint inum, void *xp, int n);
static void winode(uint inum, struct dinode *ip);
static void rinode(uint inum, struct dinode *ip);

/* --- 工具宏 --- */
#define NINODES 200
#define IPB (BSIZE / sizeof(struct dinode))  /* 每块 inode 数 */

/* 统一使用小端序写入（宿主机可能是大端）*/
static uint xint(uint x) { return x; }
static ushort xshort(ushort x) { return x; }

static void wsect(uint sec, void *buf) {
  if(lseek(fsfd, (long)sec * BSIZE, SEEK_SET) != (long)sec * BSIZE) {
    perror("lseek"); exit(1);
  }
  if(write(fsfd, buf, BSIZE) != BSIZE) {
    perror("write"); exit(1);
  }
}

static void rsect(uint sec, void *buf) {
  if(lseek(fsfd, (long)sec * BSIZE, SEEK_SET) != (long)sec * BSIZE) {
    perror("lseek"); exit(1);
  }
  if(read(fsfd, buf, BSIZE) != BSIZE) {
    perror("read"); exit(1);
  }
}

static uint freeblock;   /* 下一个空闲数据块 */
static uint freeinode = 1; /* 下一个空闲 inode（从1开始，0保留）*/

/* 在位图中标记 blockno 已被使用 */
static void balloc_mark(uint blockno) {
  uchar buf[BSIZE];
  uint bmapblock = sb.bmapstart + blockno / (BSIZE * 8);
  rsect(bmapblock, buf);
  buf[(blockno % (BSIZE * 8)) / 8] |= (1 << (blockno % 8));
  wsect(bmapblock, buf);
}

/* 分配并返回一个新数据块 */
static uint next_block(void) {
  uint b = freeblock++;
  balloc_mark(b);
  return b;
}

static void winode(uint inum, struct dinode *ip) {
  uchar buf[BSIZE];
  uint bn = sb.inodestart + inum / IPB;
  rsect(bn, buf);
  struct dinode *dip = (struct dinode *)buf + (inum % IPB);
  *dip = *ip;
  wsect(bn, buf);
}

static void rinode(uint inum, struct dinode *ip) {
  uchar buf[BSIZE];
  uint bn = sb.inodestart + inum / IPB;
  rsect(bn, buf);
  struct dinode *dip = (struct dinode *)buf + (inum % IPB);
  *ip = *dip;
}

static uint ialloc(ushort type) {
  uint inum = freeinode++;
  struct dinode din;
  memset(&din, 0, sizeof(din));
  din.type = xshort(type);
  din.nlink = xshort(1);
  din.size = xint(0);
  winode(inum, &din);
  return inum;
}

static void iappend(uint inum, void *xp, int n) {
  char *p = (char *)xp;
  struct dinode din;
  uint fbn;
  rinode(inum, &din);
  uint off = xint(din.size);

  while(n > 0) {
    fbn = off / BSIZE;
    assert(fbn < NDIRECT + NINDIRECT);

    uint x;
    if(fbn < NDIRECT) {
      if(xint(din.addrs[fbn]) == 0) {
        din.addrs[fbn] = xint(next_block());
      }
      x = xint(din.addrs[fbn]);
    } else {
      /* 间接块 */
      if(xint(din.addrs[NDIRECT]) == 0) {
        din.addrs[NDIRECT] = xint(next_block());
      }
      uint indirect_blk = xint(din.addrs[NDIRECT]);
      uint ibuf[BSIZE / sizeof(uint)];
      rsect(indirect_blk, ibuf);
      if(xint(ibuf[fbn - NDIRECT]) == 0) {
        ibuf[fbn - NDIRECT] = xint(next_block());
        wsect(indirect_blk, ibuf);
      }
      x = xint(ibuf[fbn - NDIRECT]);
    }

    uint n1 = BSIZE - (off % BSIZE);
    if(n1 > (uint)n) n1 = n;

    uchar buf[BSIZE];
    rsect(x, buf);
    memcpy(buf + (off % BSIZE), p, n1);
    wsect(x, buf);

    n -= n1;
    off += n1;
    p += n1;
  }

  din.size = xint(off);
  winode(inum, &din);
}

int main(int argc, char *argv[]) {
  if(argc < 2) {
    fprintf(stderr, "Usage: mkfs <image> [nblocks]\n");
    exit(1);
  }

  int total = FSSIZE;
  if(argc >= 3) total = atoi(argv[2]);

  /* 计算布局 */
  nbitmap = (total + BSIZE*8 - 1) / (BSIZE*8);
  ninodes = NINODES;
  int inodeblocks = (ninodes + IPB - 1) / IPB;
  nmeta = 1 + 1 + inodeblocks + nbitmap;  /* boot + sb + inodes + bmap */
  nblocks = total - nmeta;

  /* 打开镜像 */
  fsfd = open(argv[1], O_RDWR | O_CREAT | O_TRUNC, 0666);
  if(fsfd < 0) { perror("open"); exit(1); }

  /* 清零整个镜像 */
  {
    uchar zeroes[BSIZE];
    memset(zeroes, 0, sizeof(zeroes));
    for(int i = 0; i < total; i++)
      wsect(i, zeroes);
  }

  /* 写超级块 */
  memset(&sb, 0, sizeof(sb));
  sb.magic      = FSMAGIC;
  sb.size       = xint(total);
  sb.nblocks    = xint(nblocks);
  sb.ninodes    = xint(ninodes);
  sb.nlog       = 0;
  sb.logstart   = 0;
  sb.inodestart = xint(2);                        /* block 2 起存 inode */
  sb.bmapstart  = xint(2 + inodeblocks);          /* 位图紧随 inode */

  uchar sbuf[BSIZE];
  memset(sbuf, 0, sizeof(sbuf));
  memcpy(sbuf, &sb, sizeof(sb));
  wsect(SBLOCK, sbuf);

  /* 数据块从元数据之后开始 */
  freeblock = nmeta;

  /* 标记元数据块已使用（块0到nmeta-1）*/
  for(int i = 0; i < nmeta; i++)
    balloc_mark(i);

  /* 创建根目录（inode 1）*/
  uint rootino = ialloc(T_DIR);
  assert(rootino == ROOTINO);

  /* 写入根目录的 "." 和 ".." 条目 */
  struct dirent de;
  memset(&de, 0, sizeof(de));
  de.inum = xshort(rootino);
  memcpy(de.name, ".", 1);
  iappend(rootino, &de, sizeof(de));

  memset(&de, 0, sizeof(de));
  de.inum = xshort(rootino);
  memcpy(de.name, "..", 2);
  iappend(rootino, &de, sizeof(de));

  /* 更新根目录 inode 大小 */
  struct dinode din;
  rinode(rootino, &din);
  printf("mkfs: created fs.img: total=%d meta=%d data=%d\n",
         total, nmeta, nblocks);
  printf("mkfs: root inode size=%u\n", xint(din.size));

  close(fsfd);
  return 0;
}

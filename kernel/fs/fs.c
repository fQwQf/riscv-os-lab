/* fs.c — 文件系统核心（Lab7）
 *
 * 本文件实现文件系统的所有核心功能，分层如下：
 *
 * 【块分配层】
 *   bzero(dev, bno)   — 清零一个磁盘块（分配新块后调用，防止旧数据泄露）
 *   balloc(dev)       — 分配一个空闲磁盘块（扫描位图，返回块号）
 *   bfree(dev, b)     — 释放一个磁盘块（位图对应位清零）
 *
 * 【inode 管理层】
 *   fsinit(dev)       — 文件系统初始化（读取超级块、清空 inode 缓存）
 *   iget(dev, inum)   — 获取 inode 的内存引用（不读磁盘！只操作 icache）
 *   ilock(ip)         — 锁定 inode 并从磁盘加载数据（valid: 0→1）
 *   iunlock(ip)       — 解锁 inode
 *   iput(ip)          — 释放 inode 引用（ref--，归零则缓存槽空闲）
 *   iupdate(ip)       — 将内存修改写回磁盘 inode
 *   ialloc(dev, type) — 在磁盘上分配一个新 inode
 *
 * 【块映射与读写层】
 *   bmap(ip, bn)      — 逻辑块号 → 物理块号（处理直接/间接索引，自动分配）
 *   readi(...)        — 从 inode 读数据
 *   writei(...)       — 向 inode 写数据
 *
 * 【目录与路径层】
 *   dirlookup(dp, name, poff) — 在目录中按文件名查找
 *   dirlink(dp, name, inum)   — 在目录中添加一条记录
 *   skipelem(path, name)      — 路径解析辅助（取一个分量）
 *   namex(path, nameiparent, name) — 路径遍历核心
 *   namei(path)               — 路径 → inode
 *   nameiparent(path, name)   — 路径 → 父目录 inode + 最后一个分量名
 */

#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "riscv.h"
#include "types.h"
#include "buf.h"
#include "fs.h"
#include "file.h"

/* 声明外部函数（由 bio.c 提供） */
extern struct buf *bread(uint dev, uint blockno);
extern void brelse(struct buf *b);
extern void bwrite(struct buf *b);
extern void binit(void);

/* ---- 内部工具函数 ---- */

static void *memset(void *dst, int c, uint n) {
  char *cdst = (char *) dst;
  for(uint i = 0; i < n; i++) cdst[i] = c;
  return dst;
}

static void *memmove(void *dst, const void *src, uint n) {
  char *d = (char *)dst;
  const char *s = (const char *)src;
  if(s < d && s + n > d){
    /* dst 和 src 有重叠，从后往前拷贝避免覆盖 */
    s += n; d += n;
    while(n-- > 0) *--d = *--s;
  } else {
    while(n-- > 0) *d++ = *s++;
  }
  return dst;
}

/* ---- 全局数据结构 ---- */

/* 超级块：文件系统的"说明书"，在 fsinit 时从磁盘块 1 读入
 * 记录了各区域（inode区、位图区、数据区）的起始位置和大小 */
struct superblock sb;

/* inode 缓存：内存中最多缓存 NINODE=50 个 inode
 * 每个 struct inode 是对应磁盘 struct dinode 的内存副本 + 运行时信息（ref, valid） */
struct {
  struct inode inode[NINODE];
} icache;

/* ================================================================
 * fsinit — 文件系统初始化（系统启动时由 start_main 调用）
 *
 * 职责：
 *   1. 读取超级块（磁盘块 1）到全局变量 sb
 *   2. 验证魔数（确保磁盘镜像有效）
 *   3. 清空 inode 缓存（所有槽位 ref=0, valid=0）
 *
 * 调用时机：start_main() 中 fsinit(ROOTDEV) —— ROOTDEV=1（virtio 磁盘）
 * ================================================================ */
void fsinit(int dev) {
  struct buf *bp;
  /* binit() 已在 virtio_disk_init() 之前由 main.c 调用过，此处跳过 */
  bp = bread(dev, 1);                   /* 读取磁盘块 1（超级块） */
  memmove(&sb, bp->data, sizeof(sb));   /* 复制到全局 sb */
  brelse(bp);
  if(sb.magic != FSMAGIC) panic("invalid file system");  /* 验证魔数 0x10203040 */
  for(int i = 0; i < NINODE; i++) {     /* 清空 inode 缓存 */
    icache.inode[i].ref = 0;
    icache.inode[i].valid = 0;
  }
}

/* ================================================================
 * bzero — 清零一个磁盘块
 *
 * 为什么需要：balloc 分配新块后立即清零，防止"前一个文件的旧数据
 * 泄露给新文件"（安全问题——旧文件可能包含敏感数据）。
 *
 * 流程：bread 读出 → memset 清零 → bwrite 写回 → brelse 释放
 * ================================================================ */
static void bzero(uint dev, int bno) {
  struct buf *bp = bread(dev, bno);     /* 读出该块 */
  memset(bp->data, 0, BSIZE);           /* 清零 1024 字节 */
  bwrite(bp);                           /* 写回磁盘 */
  brelse(bp);                           /* 释放缓冲块 */
}

/* ================================================================
 * balloc — 分配一个空闲磁盘块
 *
 * 算法：扫描位图区，找到第一个 bit==0 的位，标记为 1（已用），返回对应块号。
 *
 * 位图中每个 bit 对应一个数据块：
 *   bit=0 → 空闲，bit=1 → 已用
 *   每个字节控制 8 个块，每个位图块（1024字节）控制 8192 个块
 *
 * 对于块号 b：
 *   它在哪个位图块：sb.bmapstart + b / (BSIZE*8)
 *   在那个位图块的哪个字节：b % (BSIZE*8) / 8
 *   在那个字节的哪一位：b % 8
 *
 * ⚠ 分配后立即调用 bzero 清零新块。
 * ================================================================ */
static uint balloc(uint dev) {
  int b, bi, m;
  struct buf *bp;
  /* 外层循环：遍历所有位图块
   * b 是"块号基数"，每次步进 BSIZE*8（一个位图块控制的块数） */
  for(b = 0; b < sb.size; b += BSIZE*8){
    bp = bread(dev, sb.bmapstart + b/(BSIZE*8));  /* 读取一个位图块 */
    for(bi = 0; bi < BSIZE && b + bi*8 < sb.size; bi++){
      m = bp->data[bi];                  /* 取一个字节 */
      if(m != 0xff){                     /* 如果不是全 1（全忙），有空间 */
        for(int bi2 = 0; bi2 < 8; bi2++){ /* 逐位检查 */
          if((m & (1 << bi2)) == 0){     /* 找到 bit==0（空闲块） */
            bp->data[bi] |= (1 << bi2);  /* 标记为已用（bit 置 1） */
            bwrite(bp);                  /* 写回位图到磁盘 */
            brelse(bp);                  /* 释放位图缓冲块 */
            bzero(dev, b + bi*8 + bi2);  /* 清零新分配的数据块 */
            return b + bi*8 + bi2;       /* 返回块号 */
          }
        }
      }
    }
    brelse(bp);  /* 该位图块全满，释放后检查下一个 */
  }
  panic("balloc: out of blocks");  /* 所有数据块都已分配 */
  return 0;
}

/* ================================================================
 * bfree — 释放一个磁盘块（将位图中对应位清零）
 *
 * 在实现文件删除（unlink）时需要调用此函数释放文件占用的数据块。
 * ================================================================ */
static void __attribute__((unused)) bfree(uint dev, uint b) {
  struct buf *bp = bread(dev, sb.bmapstart + b/(BSIZE*8));  /* 读位图块 */
  int bi = (b % (BSIZE*8)) / 8;    /* 块号对应的字节索引 */
  int m = 1 << (b % 8);            /* 块号对应的位掩码 */
  if((bp->data[bi] & m) == 0) panic("freeing free block");  /* 重复释放检测 */
  bp->data[bi] &= ~m;              /* 对应位清零 */
  bwrite(bp);                      /* 写回位图 */
  brelse(bp);
}

/* ================================================================
 * iget — 获取 inode 的内存引用（不读磁盘！）
 *
 * ⚠ 核心设计：iget 只在 icache 中操作——找一个空闲槽或复用已有槽。
 *    它永远不会触发磁盘 I/O。磁盘读取是 ilock 的事。
 *
 * 为什么这样设计？
 *   如果 iget 直接读磁盘，两个进程同时 iget 同一 inode 会：
 *   1. 重复读磁盘（性能浪费）
 *   2. 第二次读可能覆盖第一次已修改但未写回的内存数据（一致性破坏）
 *
 * 算法：
 *   扫描 icache.inode[]：
 *     分支 A：找到 dev==dev && inum==inum && ref>0 → 命中缓存，ref++，返回
 *     分支 B：同时记录第一个 ref==0 的空槽
 *   扫描结束后：
 *     若未命中但有空槽 → 用空槽：填入 dev, inum, ref=1, valid=0
 *     若既没命中也没空槽 → panic
 *
 * ⚠ 正确使用顺序：
 *    ip = iget(dev, inum);   // 只找缓存槽
 *    ilock(ip);              // 现在才从磁盘加载数据
 *    ... 使用 ip->addrs[], ip->size 等 ...
 *    iunlock(ip);
 *    iput(ip);               // 释放引用
 * ================================================================ */
struct inode *iget(uint dev, uint inum) {
  struct inode *ip, *empty = 0;
  /* 单次遍历同时完成两件事：
   * 1. 查找是否已有该 inode 的缓存（命中）
   * 2. 记录第一个空闲槽位（备用） */
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;    /* 命中：引用计数+1，不重置 valid（已有缓存继续使用） */
      return ip;
    }
    if(empty == 0 && ip->ref == 0) empty = ip;  /* 记录备用空槽 */
  }
  /* 未命中，使用空槽分配新的缓存项 */
  if(empty == 0) panic("iget: no inodes");
  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;      /* 引用计数初始化为 1 */
  ip->valid = 0;    /* 数据尚未从磁盘加载，需要 ilock 时才加载 */
  return ip;
}

/* 每个磁盘块能容纳的 inode 数量（用于计算 inode 在磁盘上的位置）
 * IPB = BSIZE / sizeof(struct dinode) = 1024 / 64 = 16 个 inode 每块 */
#define IPB (BSIZE / sizeof(struct dinode))

/* ================================================================
 * ilock — 锁定 inode 并从磁盘加载数据
 *
 * iget 返回的 inode 可能 valid==0（数据还在磁盘上，没加载）。
 * ilock 负责：若 valid==0，从磁盘读取对应的 struct dinode，复制字段到内存 inode。
 *
 * inode 编号与磁盘位置的对应关系：
 *   blockno = sb.inodestart + inum / IPB    （该 inode 在哪个磁盘块）
 *   offset  = inum % IPB                    （在块内的第几个 dinode）
 *
 * 这两个公式在 ilock 和 iupdate 中频繁使用。
 * ================================================================ */
void ilock(struct inode *ip) {
  struct buf *bp;
  struct dinode *dip;
  if(ip == 0 || ip->ref < 1) panic("ilock");

  if(ip->valid == 0){
    /* valid==0：磁盘数据尚未加载到内存，需要读取 */
    uint blockno = sb.inodestart + ip->inum / IPB;  /* 计算 inode 所在磁盘块 */
    bp = bread(ip->dev, blockno);                    /* 读出该块 */
    dip = (struct dinode*)bp->data + ip->inum % IPB; /* 定位到具体的 dinode */

    /* 将磁盘 dinode 的字段复制到内存 inode */
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));

    brelse(bp);        /* 释放缓冲块 */
    ip->valid = 1;     /* 标记数据已加载 */

    if(ip->type == 0)
      panic("ilock: no type");  /* type==0 表示该 inode 空闲，不应被使用 */
  }
}

/* ================================================================
 * iunlock — 解锁 inode
 *
 * 简化版中不做真正的锁操作（无睡眠锁实现），
 * 只做基本的合法性检查。
 * ================================================================ */
void iunlock(struct inode *ip) {
  if(ip == 0 || ip->ref < 1) panic("iunlock");
}

/* ================================================================
 * iput — 释放 inode 引用（"我用完了，还给你"）
 *
 * ref--，若归零说明没有人再持有这个 inode：
 *   - valid=0：标记缓存槽空闲，供下次 iget 重用
 *   - 若 nlink==0 且 ref==1：文件已被 unlink 且无人引用，
 *     应释放所有数据块（简化版暂不实现 itrunc）
 *
 * ⚠ 完整版（如 xv6）在 ref==0 && nlink==0 时会调用 itrunc
 *    释放所有数据块和 inode 自身。本实验简化为只做引用计数管理。
 * ================================================================ */
void iput(struct inode *ip) {
  if(ip->ref == 1 && ip->valid && ip->nlink == 0) {
    /* nlink==0 表示文件已被 unlink，但 ref==1 说明当前还有人持有
     * 这是要释放的最后时机（简化版只清除 valid） */
    ip->valid = 0;
  }
  ip->ref--;
  if(ip->ref == 0) ip->valid = 0;  /* ref 归零，缓存槽空闲 */
}

/* ================================================================
 * iupdate — 将内存 inode 的修改写回磁盘
 *
 * 与 ilock 方向相反：
 *   ilock:  磁盘 dinode → 内存 inode（读入）
 *   iupdate: 内存 inode → 磁盘 dinode（写回）
 *
 * 调用场景：writei 修改了 ip->size 或 ip->addrs[] 后，
 *           必须调用 iupdate 将修改持久化到磁盘。
 * ================================================================ */
void iupdate(struct inode *ip) {
  /* 读取 inode 所在的磁盘块 */
  struct buf *bp = bread(ip->dev, sb.inodestart + ip->inum / IPB);
  struct dinode *dip = (struct dinode*)bp->data + ip->inum % IPB;

  /* 将内存 inode 的字段写回磁盘 dinode */
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));

  bwrite(bp);   /* 写回磁盘 */
  brelse(bp);
}

/* ================================================================
 * ialloc — 在磁盘上分配一个新 inode
 *
 * 调用场景：sys_open(O_CREAT) 创建新文件时需要分配 inode。
 *
 * 算法：
 *   遍历磁盘 inode 区（从 inum=1 开始，inum=0 保留），
 *   找到第一个 type==0 的 dinode（空闲 inode），
 *   将其类型设置为指定 type，写回磁盘，返回对应的内存 inode。
 *
 * ⚠ 返回的是 iget 的结果（内存 inode，valid 可能为 0）。
 *    调用者需要 ilock 后才能使用其字段。
 * ================================================================ */
struct inode *ialloc(uint dev, short type) {
  for(int inum = 1; inum < sb.ninodes; inum++){
    /* 读取包含第 inum 个 inode 的磁盘块 */
    struct buf *bp = bread(dev, sb.inodestart + inum / IPB);
    struct dinode *dip = (struct dinode*)bp->data + inum % IPB;
    if(dip->type == 0){       /* type==0 表示该 inode 空闲 */
      memset(dip, 0, sizeof(*dip));  /* 清零整个 dinode */
      dip->type = type;       /* 占用并设置类型（T_FILE / T_DIR） */
      bwrite(bp);             /* 写回磁盘：新 inode 已分配 */
      brelse(bp);
      return iget(dev, inum); /* 返回对应的内存 inode 缓存项 */
    }
    brelse(bp);               /* 该 inode 不空闲，继续检查下一个 */
  }
  panic("ialloc: no inodes");  /* 所有 inode 都已分配 */
  return 0;
}

/* ================================================================
 * bmap — 将文件的逻辑块号映射到磁盘的物理块号
 *
 * 核心问题：文件的第 bn 个逻辑块，实际存在磁盘的哪个物理块？
 *
 * 映射规则（两级结构）：
 *   bn < NDIRECT(12)        → 直接映射：ip->addrs[bn]
 *   bn < NDIRECT + NINDIRECT → 间接映射：先读间接块 ip->addrs[NDIRECT]，
 *                                再在其中找 a[bn - NDIRECT]
 *   否则                    → 文件过大，panic
 *
 * ⚠ 若目标块尚未分配（地址为 0），bmap 自动调用 balloc 分配新块。
 *    这意味着第一次 writei 某个逻辑块时，bmap 会"按需分配"物理块。
 *
 * 最大文件大小 = (12 + 256) × 1024 = 268 KB
 *   12 个直接块 + 1 个间接块（内含 256 个块号）
 * ================================================================ */
static uint bmap(struct inode *ip, uint bn) {
  uint addr;
  struct buf *bp;
  uint *a;

  /* ---- 直接映射：前 NDIRECT=12 个逻辑块 ---- */
  if (bn < NDIRECT) {
    if ((addr = ip->addrs[bn]) == 0) {
      /* 该逻辑块尚未分配物理块 → 分配一个 */
      ip->addrs[bn] = addr = balloc(ip->dev);
    }
    return addr;  /* 返回物理块号 */
  }

  bn -= NDIRECT;  /* 转换为间接块内的索引 */

  /* ---- 一级间接映射：逻辑块 12 ~ 267 ---- */
  if (bn < NINDIRECT) {
    /* 先确保间接指针块本身已分配
     * ip->addrs[NDIRECT] 存的不是数据，而是一个"块号表"所在的块号
     * 这个块里面有 256 个 uint，每个指向一个数据块 */
    if ((addr = ip->addrs[NDIRECT]) == 0) {
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    }

    /* 读取间接指针块（它里面存的是一堆物理块地址） */
    bp = bread(ip->dev, addr);
    a = (uint *)bp->data;  /* 把 data 当作 uint 数组来访问 */

    /* 在间接块中查找第 bn 个物理块地址 */
    if ((addr = a[bn]) == 0) {
      a[bn] = addr = balloc(ip->dev);  /* 分配新的数据块 */
      bwrite(bp);  /* ⚠ 必须写回！间接块的修改丢失 = 数据块地址丢失 */
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");  /* 超出最大文件大小 */
}

/* ================================================================
 * readi — 从 inode 文件中读取数据
 *
 * 参数：
 *   ip       — 要读取的 inode（必须已 ilock）
 *   user_dst — 目标地址是否是用户虚拟地址（1=用户，0=内核）
 *              本框架恒等映射下，两者等价，直接 memmove 即可
 *   dst      — 目标缓冲区地址
 *   off      — 文件内偏移（字节）
 *   n        — 要读取的字节数
 *
 * 返回：实际读取的字节数（off 超过文件末尾返回 0）
 *
 * 核心逻辑：
 *   因为一次读请求可能跨越多个磁盘块（比如从字节 1020 读 100 字节
 *   横跨第 0 块和第 1 块），所以用循环逐块处理：
 *     每次迭代：
 *       1. bmap(ip, off/BSIZE) 获取当前逻辑块对应的物理块号
 *       2. bread 读出该物理块
 *       3. memmove 从 buf->data 中拷贝本次需要的字节到 dst
 *       4. brelse 释放缓冲块
 * ================================================================ */
int readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n) {
  uint tot, m;
  struct buf *bp;

  /* 边界检查 */
  if (off > ip->size || off + n < off)  /* 超出文件末尾或整数溢出 */
    return 0;
  if (off + n > ip->size)
    n = ip->size - off;  /* 截断到文件末尾 */

  /* 逐块读取循环 */
  for (tot = 0; tot < n; tot += m, off += m, dst += m) {
    /* 找到当前偏移所在的物理磁盘块 */
    uint blockno = bmap(ip, off / BSIZE);  /* off/BSIZE = 逻辑块号 */
    bp = bread(ip->dev, blockno);           /* 读出该物理块 */

    /* 计算本次从这一块可以读多少字节
     * m = 剩余块内字节数（BSIZE - 块内偏移）和 剩余需读字节数（n-tot）的较小值 */
    m = BSIZE - off % BSIZE;
    if (m > n - tot)
      m = n - tot;

    /* 从缓冲块中拷贝数据到目标地址
     * off%BSIZE 是块内偏移——文件字节偏移 off 对应块内的第几个字节 */
    memmove((void*)dst, bp->data + off % BSIZE, m);

    brelse(bp);  /* 释放缓冲块 */
  }

  return (int)tot;  /* 返回实际读取的字节数 */
}

/* ================================================================
 * dirlookup — 在目录 inode 中按文件名查找
 *
 * 原理：目录也是文件！它的"文件内容"就是一条条 struct dirent 记录：
 *   struct dirent { ushort inum; char name[14]; }  // 共 16 字节
 *
 * dirlookup 用 readi 遍历目录的每一条 dirent，比较名字字段。
 *
 * 参数：
 *   dp   — 目录的 inode 指针
 *   name — 要查找的文件名
 *   poff — （可选输出）找到该条目在目录中的字节偏移
 *
 * 返回：找到则返回对应 inode 的指针（调用 iget）；未找到返回 0。
 *
 * ⚠ 名字比较用固定字节数（DIRSIZ=14），不是 strcmp！
 *    因为 name[14] 数组中未使用的部分是未定义的（可能是垃圾值）。
 *    只能逐字节固定长度比较，遇到 name[i]=='\0' 同时停止即可。
 * ================================================================ */
struct inode *dirlookup(struct inode *dp, char *name, uint *poff) {
  uint off, inum;
  struct dirent de;

  /* 逐条读取目录中的 dirent 记录（每条 16 字节） */
  for (off = 0; off < dp->size; off += sizeof(de)) {
    /* 从目录文件中读取一条记录 */
    if (readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup: read error");

    /* inum==0 表示这个槽位是空的（文件已删除），跳过 */
    if (de.inum == 0)
      continue;

    /* 名字比较：逐字节固定长度比较（最多 DIRSIZ=14 个字符）
     * 遇到不等的字符 → 不匹配
     * 遇到 '\0' → 两者同时到达结尾，匹配 */
    int match = 1;
    for(int i = 0; i < DIRSIZ; i++) {
      if(de.name[i] != name[i]) { match = 0; break; }
      if(name[i] == 0) break;
    }
    if (match) {
      if (poff) *poff = off;    /* 输出偏移（给 dirlink 等使用） */
      inum = de.inum;           /* 记录 inode 编号 */
      return iget(dp->dev, inum);  /* 返回对应的内存 inode */
    }
  }

  return 0;  /* 未找到 */
}

/* ================================================================
 * writei — 向 inode 文件写入数据
 *
 * 与 readi 镜像对称，关键差异：
 *   1. memmove 方向反过来：从 src 拷到 buf->data[]（而非从 buf 拷到 dst）
 *   2. 修改缓冲块后必须 bwrite(bp) 写回磁盘
 *   3. 写入可能使文件变大：若 off+n > ip->size，更新 ip->size
 *   4. 无论大小是否变化，结束后必须 iupdate(ip) 将修改写回磁盘 inode
 *
 * 参数：
 *   user_src — 源地址是否是用户虚拟地址（简化版下等价于内核地址）
 *   src      — 源数据地址
 *   off      — 文件内写入偏移（字节）
 *   n        — 要写入的字节数
 * ================================================================ */
int writei(struct inode *ip, int user_src, uint64 src, uint off, uint n) {
  uint tot, m;
  struct buf *bp;

  /* 边界检查 */
  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > (NDIRECT + NINDIRECT) * BSIZE)  /* 超出最大文件大小 */
    return -1;

  /* 逐块写入循环 */
  for(tot = 0; tot < n; tot += m, off += m, src += m){
    uint blockno = bmap(ip, off / BSIZE);  /* 逻辑块号 → 物理块号（可能触发 balloc） */
    bp = bread(ip->dev, blockno);           /* 读出该物理块 */
    m = BSIZE - off % BSIZE;               /* 本次可写的最大字节数 */
    if(m > n - tot)
      m = n - tot;

    /* 将数据从 src 拷贝到缓冲块的对应位置 */
    memmove(bp->data + off % BSIZE, (void*)src, m);
    bwrite(bp);    /* ⚠ 必须写回磁盘！否则修改只留在内存中 */
    brelse(bp);
  }

  /* 写入可能使文件变大，需要更新 size 并持久化 */
  if(n > 0 && off > ip->size){
    ip->size = off;   /* 更新文件大小 */
    iupdate(ip);      /* 写回磁盘 inode（更新 size 和可能新增的 addrs） */
  }
  return n;
}

/* ================================================================
 * dirlink — 在目录中添加一条记录（创建新文件时的关键步骤）
 *
 * 调用场景：sys_open(O_CREAT) → ialloc 分配新 inode 后，
 *           调用 dirlink 在父目录中写入 "filename → inum" 的映射。
 *
 * 算法：
 *   1. 先调 dirlookup 检查是否已存在同名文件（已存在则返回 -1）
 *   2. 遍历目录找空槽（de.inum == 0），或追加到末尾
 *   3. 填写 de.name 和 de.inum
 *   4. 调用 writei 将这条 dirent 写回磁盘
 *
 * ⚠ memset(de.name, 0, DIRSIZ) 必须先清零再复制名字，
 *    否则残留的垃圾数据会导致 dirlookup 的比较行为不一致。
 * ================================================================ */
int dirlink(struct inode *dp, char *name, uint inum) {
  int off;
  struct dirent de;
  struct inode *ip;

  /* 检查是否已存在同名文件 */
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);   /* dirlookup 返回时 ref+1 了，需要 iput 释放 */
    return -1;  /* 文件已存在 */
  }

  /* 遍历目录找空槽（de.inum == 0） */
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)  /* 找到空槽 */
      break;
  }

  /* 填写新目录项 */
  memset(de.name, 0, DIRSIZ);  /* ⚠ 先清零！防止残留垃圾 */
  for(int i = 0; i < DIRSIZ && name[i]; i++)
    de.name[i] = name[i];      /* 复制文件名 */
  de.inum = inum;              /* 设置 inode 编号 */

  /* 写回目录文件 */
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink write");

  return 0;
}

/* ================================================================
 * skipelem — 路径解析辅助函数
 *
 * 从路径字符串中提取下一个分量（两个 '/' 之间的部分）。
 *
 * 示例：
 *   skipelem("/etc/passwd", name) → name="etc", 返回 "/passwd"
 *   skipelem("/passwd", name)     → name="passwd", 返回 ""（空串，表示还有最后一个分量）
 *   skipelem("", name)            → 返回 0（路径结束）
 *   skipelem("/usr//local", name) → name="usr", 返回 "/local"（跳过连续斜杠）
 *
 * ⚠ 三个边界条件：
 *   1. 正常路径：跳过前导 '/'，提取下一个 '/' 之前的内容
 *   2. 尾部斜杠："/etc/" → name="etc", 返回 ""（不是 0！）
 *   3. 空字符串：返回 0（路径结束标志）
 * ================================================================ */
static char* skipelem(char *path, char *name) {
  char *s;
  int len;

  /* 跳过开头的所有 '/' */
  while(*path == '/')
    path++;
  if(*path == 0)    /* 跳过后为空，说明路径结束 */
    return 0;

  s = path;
  /* 找到下一个 '/' 或字符串结尾 */
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;   /* 分量的长度 */

  /* 复制分量名到 name */
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);  /* 超长则截断到 DIRSIZ */
  else {
    memmove(name, s, len);
    name[len] = 0;  /* 正常长度则在末尾加 '\0' */
  }

  /* 跳过分量后面的所有 '/' */
  while(*path == '/')
    path++;

  return path;  /* 返回剩余路径（空串表示这是最后一个分量） */
}

/* ================================================================
 * namex — 路径遍历核心（namei 和 nameiparent 都调用它）
 *
 * 算法：
 *   1. 若 path[0]=='/'：从根目录开始（iget(ROOTDEV, ROOTINO)）
 *      否则：也应从当前进程工作目录开始（简化版直接用根目录）
 *   2. 循环：
 *      a. skipelem 取下一个路径分量
 *      b. 若 path==0：已到终点
 *         - nameiparent 模式：返回当前 ip（最后一个分量之前的目录）
 *         - namei 模式：返回当前 ip（目标文件的 inode）
 *      c. ilock(ip) — 准备读取目录内容
 *      d. 检查 ip->type==T_DIR — 当前节点必须是目录才能继续遍历
 *      e. dirlookup 在当前目录中查找 name
 *      f. iunlock + iput(ip) — 释放当前层级
 *      g. ip = next — 进入下一层级
 *
 * 参数：
 *   path          — 路径字符串
 *   nameiparent   — 1=返回父目录 inode，0=返回目标 inode
 *   name          — 输出最后一个路径分量名
 * ================================================================ */
static struct inode* namex(char *path, int nameiparent, char *name) {
  struct inode *ip, *next;

  /* 确定起始目录（简化版：始终从根目录开始） */
  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = iget(ROOTDEV, ROOTINO);  /* TODO: 应使用进程的 cwd */

  /* 逐级遍历路径 */
  while((path = skipelem(path, name)) != 0){
    ilock(ip);  /* 锁定当前 inode，准备读取 */

    /* 当前节点必须是目录才能继续遍历 */
    if(ip->type != T_DIR){
      iunlock(ip);
      iput(ip);
      return 0;  /* 路径中间某级不是目录，非法路径 */
    }

    /* nameiparent 模式：如果这是最后一个分量，返回当前目录（父目录） */
    if(nameiparent && *path == '\0'){
      iunlock(ip);
      return ip;  /* 返回父目录 inode，name 中是文件名 */
    }

    /* 在当前目录中查找 name 分量 */
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlock(ip);
      iput(ip);
      return 0;  /* 未找到该文件/目录 */
    }

    iunlock(ip);
    iput(ip);     /* 释放当前层级 */
    ip = next;    /* 进入下一层级 */
  }

  /* nameiparent 模式下，路径遍历结束后还没取到最后一个分量就结束了
   * 这意味着路径以 '/' 结尾或路径为空，返回失败 */
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;  /* namei 模式：返回目标文件的 inode */
}

/* ================================================================
 * namei — 将路径字符串转换为 inode 指针
 *
 * 示例：
 *   namei("/")          → 返回根目录 inode
 *   namei("/hello.txt") → 返回 hello.txt 的 inode
 *   namei("/nonexist")  → 返回 0
 *
 * 调用者：sys_open（非 O_CREAT 模式）、sys_read、sys_write 等
 * ================================================================ */
struct inode* namei(char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);  /* nameiparent=0：返回目标本身 */
}

/* ================================================================
 * nameiparent — 将路径转换为父目录 inode + 最后一个分量名
 *
 * 示例：
 *   nameiparent("/etc/passwd", name) → 返回 etc 目录的 inode, name="passwd"
 *   nameiparent("/hello.txt", name)  → 返回根目录的 inode, name="hello.txt"
 *
 * 调用者：sys_open(O_CREAT) — 需要在父目录中创建新文件
 * ================================================================ */
struct inode* nameiparent(char *path, char *name) {
  return namex(path, 1, name);  /* nameiparent=1：返回父目录 */
}

/* file.c — 文件描述符层（Lab7）
 *
 * 【这一层解决了什么问题？】
 * 用户进程打开文件后拿到的不是 inode 指针，而是一个整数 fd。
 * fd 背后隐藏着：
 *   - 当前读写的偏移量 off（两个进程打开同一文件，各自独立偏移）
 *   - 这个文件的访问模式（只读 / 只写 / 读写）
 *   - 指向对应 inode 的指针
 *
 * 【为什么不能让 fd 直接对应 inode？】
 * 因为 inode 是磁盘上文件的"身份标识"，全局唯一。
 * 而文件描述符是进程打开文件的"使用实例"。
 * 同一文件可以被一个进程打开多次，每次得到不同的 fd，
 * 各自维护自己的 off 偏移。省略这层会导致多进程无法独立偏移。
 *
 * 【数据结构】
 *   struct file — 一个打开文件的实例（内存中，不属于磁盘）
 *     type:      FD_NONE=未使用, FD_INODE=inode文件
 *     ref:       引用计数（fork 后父子共享同一 file 时 ref>1）
 *     readable:  是否可读
 *     writable:  是否可写
 *     ip:        指向对应的 inode
 *     off:       当前读写偏移（字节）
 *
 *   ftable — 全局文件表（系统中所有 struct file 对象，最多 NFILE=100 个）
 *
 * 【调用关系】
 *   sys_read → fileread → readi（inode 层）
 *   sys_write → filewrite → writei（inode 层）
 *   sys_close → fileclose → iput（inode 层）
 *   sys_open → filealloc（从 ftable 分配空闲 file）
 */
#include "defs.h"
#include "param.h"
#include "proc.h"
#include "types.h"
#include "file.h"

/* 全局文件表：系统中所有打开的文件对象
 * ftable.file[i].ref == 0 表示该槽位空闲，可被 filealloc 分配 */
struct {
  struct file file[NFILE];
} ftable;

/* ================================================================
 * filealloc — 从全局文件表中分配一个空闲 file 对象
 *
 * 算法：遍历 ftable.file[]，找第一个 ref==0 的槽位，ref=1，返回
 *
 * 调用者：sys_open（创建/打开文件时需要分配 file 对象）
 * ================================================================ */
struct file *filealloc(void) {
  for(int i = 0; i < NFILE; i++){
    if(ftable.file[i].ref == 0){
      ftable.file[i].ref = 1;  /* 标记为已占用 */
      return &ftable.file[i];  /* 返回 file 指针 */
    }
  }
  return 0;  /* 文件表满 */
}

/* ================================================================
 * filedup — 增加一个 file 的引用计数（fork 时使用）
 *
 * fork 后子进程复制父进程的 ofile[]，指向同一个 struct file。
 * 此时 filedup 将 ref+1，表示多了一个持有者。
 * 当 fileclose 被调用时，只有 ref 归零才真正释放。
 * ================================================================ */
struct file *filedup(struct file *f) {
  if(f->ref < 1)
    panic("filedup");
  f->ref++;   /* 引用计数+1 */
  return f;   /* 返回同一个 file 指针 */
}

/* ================================================================
 * fileclose — 关闭文件，释放引用
 *
 * 算法：
 *   1. ref--（减少引用计数）
 *   2. 若 ref > 0：还有别人在用（如 fork 后的子进程），直接返回
 *   3. 若 ref 归零：没人再用了
 *      a. 暂存 ip = f->ip
 *      b. 清零 file 结构（type=0, ip=0, off=0 ...）
 *      c. iput(ip) 释放 inode 引用
 *
 * ⚠ 必须先暂存 ip 再清零 f->ip，否则清零后就找不到 inode 了。
 * ================================================================ */
void fileclose(struct file *f) {
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0)  /* ref 减 1 后仍 > 0，说明还有持有者（如 fork 的子进程） */
    return;

  /* ref 归零：这是最后一个持有者，真正释放资源 */
  struct inode *ip = f->ip;  /* 先暂存 inode 指针 */
  f->type = FD_NONE;         /* 标记槽位为空闲 */
  f->ip = 0;
  f->off = 0;
  f->readable = 0;
  f->writable = 0;
  if(ip){
    iput(ip);  /* 释放 inode 引用（ref--，若归零则缓存槽标记为空闲） */
  }
}

/* ================================================================
 * fileread — 从文件读取数据（文件描述符层）
 *
 * 流程：
 *   1. 检查 f->readable（文件是否以读模式打开）
 *   2. ilock(f->ip) — 锁定 inode，确保磁盘数据已加载到内存
 *   3. readi() — 从 inode 读取数据到 addr（用户缓冲区）
 *   4. f->off += r — 前进偏移量
 *   5. iunlock(f->ip) — 解锁
 *
 * 返回：实际读取的字节数，或 -1（出错）
 * ================================================================ */
int fileread(struct file *f, uint64 addr, int n) {
  int r;
  if(f->readable == 0)
    return -1;        /* 文件不可读 */
  ilock(f->ip);       /* 锁定 inode（若 valid==0 则从磁盘加载） */
  r = readi(f->ip, 1, addr, f->off, n);  /* readi: inode 层读取，user_dst=1 */
  if(r > 0)
    f->off += r;      /* 读取成功，偏移量前进 */
  iunlock(f->ip);     /* 解锁 */
  return r;
}

/* ================================================================
 * filewrite — 向文件写入数据（文件描述符层）
 *
 * 与 fileread 镜像对称：
 *   - 检查 writable（而非 readable）
 *   - 调用 writei（而非 readi）
 *   - 同样维护 f->off 偏移
 *
 * ⚠ writei 内部会：
 *   1. bmap 翻译逻辑块号（可能触发 balloc 分配新磁盘块）
 *   2. bread 读出目标块
 *   3. memmove 拷贝数据到 buf->data[]
 *   4. bwrite 写回磁盘
 *   5. iupdate 更新 inode 的 size/addrs
 * ================================================================ */
int filewrite(struct file *f, uint64 addr, int n) {
  int r;
  if(f->writable == 0)
    return -1;        /* 文件不可写 */
  ilock(f->ip);
  r = writei(f->ip, 1, addr, f->off, n);  /* writei: inode 层写入，user_src=1 */
  if(r > 0)
    f->off += r;      /* 写入成功，偏移量前进 */
  iunlock(f->ip);
  return r;
}

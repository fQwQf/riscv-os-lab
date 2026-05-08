/* sysfile.c — 文件相关系统调用（Lab7）
 *
 * 本文件实现四个系统调用的内核侧：
 *   sys_open  — 打开或创建文件
 *   sys_read  — 从文件读取数据
 *   sys_write — 向文件写入数据
 *   sys_close — 关闭文件描述符
 *
 * 【系统调用的工作原理】
 * 用户程序在 usys.S 中通过 ecall 陷入内核，
 * 内核在 syscall.c 中根据 a7 寄存器的系统调用号分发到这里的对应函数。
 * 参数不是通过 C 函数参数传递的，而是存在 trapframe 的 a0/a1/a2 寄存器中，
 * 通过 argint/argaddr/argstr 提取。
 *
 * 【调用链】
 *   sys_open  → namei/nameiparent → dirlookup/ialloc/dirlink → filealloc
 *   sys_read  → fileread → readi → bmap → bread
 *   sys_write → filewrite → writei → bmap → balloc/bread/bwrite
 *   sys_close → fileclose → iput
 */
#include "defs.h"
#include "param.h"
#include "proc.h"
#include "types.h"
#include "file.h"

/* ---- 文件打开标志常量 ---- */
#define O_RDONLY 0x000   /* 只读 */
#define O_WRONLY 0x001   /* 只写 */
#define O_RDWR   0x002   /* 读写 */
#define O_CREAT  0x200   /* 若文件不存在则创建 */
#define O_TRUNC  0x400   /* 截断文件为 0 字节 */

/* ================================================================
 * sys_open — 打开或创建文件
 *
 * 从 trapframe 读取参数：
 *   a0 = path（文件路径字符串的用户虚拟地址）
 *   a1 = omode（打开模式：O_RDONLY / O_WRONLY / O_CREAT 等）
 *
 * 完整流程（以 open("/hello.txt", O_CREAT|O_WRONLY) 为例）：
 *
 * 【O_CREAT 路径——创建新文件】
 *   1. argstr(0, path) + argint(1, &omode)  提取参数
 *   2. nameiparent(path, name)              分离父目录和文件名
 *      → 返回根目录 inode, name = "hello.txt"
 *   3. ilock(ip) + dirlookup(ip, name, 0)   在父目录中查找
 *      → 文件不存在（返回 0）
 *   4. ialloc(ip->dev, T_FILE)              分配新 inode
 *   5. dirlink(ip, name, next->inum)        在父目录中注册新文件名
 *   6. filealloc()                          分配 struct file
 *   7. 在 p->ofile[] 中找空槽 → fd
 *   8. 初始化 file：type/ip/off/readable/writable
 *   9. 返回 fd
 *
 * 【非 O_CREAT 路径——打开已有文件】
 *   1. namei(path) → 直接查找文件
 *   2. 检查权限（不能以写模式打开目录等）
 *   3. 后续步骤同上 6-9
 * ================================================================ */
uint64 sys_open(void) {
  char path[MAXPATH];
  int fd, omode;
  struct file *f;
  struct inode *ip;
  struct proc *p = myproc();

  /* 从 trapframe 提取参数 */
  if(argstr(0, path, MAXPATH) < 0 || argint(1, &omode) < 0)
    return -1;

  if(omode & O_CREAT){
    /* ---- O_CREAT 路径：可能需要创建新文件 ---- */
    char name[DIRSIZ];
    /* nameiparent：找到父目录 inode，分离出文件名
     * 例：path="/hello.txt" → ip=根目录inode, name="hello.txt" */
    ip = nameiparent(path, name);
    if(ip == 0) return -1;  /* 路径中的目录不存在 */
    ilock(ip);  /* 锁定父目录，从磁盘加载数据 */

    /* 在父目录中查找文件是否已存在 */
    struct inode *next;
    if((next = dirlookup(ip, name, 0)) != 0){
      /* ---- 文件已存在 ---- */
      iunlock(ip);
      iput(ip);         /* 释放父目录引用 */
      ip = next;        /* 切换到已存在的文件 inode */
      ilock(ip);        /* 锁定并加载文件 inode */
      /* 安全检查：不能以写模式打开目录或设备文件 */
      if(ip->type == T_DIR || ip->type == T_DEVICE){
        iunlock(ip);
        iput(ip);
        return -1;
      }
    } else {
      /* ---- 文件不存在，创建新文件 ---- */
      next = ialloc(ip->dev, T_FILE);  /* 分配新 inode */
      if(next == 0) panic("sys_open: ialloc");
      /* 在父目录中注册新文件名 → inum 的映射 */
      if(dirlink(ip, name, next->inum) < 0) panic("sys_open: dirlink");
      iunlock(ip);
      iput(ip);         /* 释放父目录 */
      ip = next;        /* 切换到新文件的 inode */
      ilock(ip);        /* 锁定新 inode */
    }
  } else {
    /* ---- 非 O_CREAT 路径：直接查找文件 ---- */
    if((ip = namei(path)) == 0) return -1;  /* 文件不存在 */
    ilock(ip);
    /* 安全检查：不能以非只读模式打开目录 */
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlock(ip);
      iput(ip);
      return -1;
    }
  }

  /* ---- 分配 file 结构和 fd ---- */
  if((f = filealloc()) == 0){
    iunlock(ip);
    iput(ip);
    return -1;  /* 文件表满 */
  }

  /* 在进程的 ofile[] 中找空闲槽位（fd 就是数组下标） */
  fd = -1;
  for(int i = 0; i < NOFILE; i++){
    if(p->ofile[i] == 0){
      p->ofile[i] = f;  /* 将 file 指针记录到进程的文件描述符表 */
      fd = i;           /* fd = 数组下标 */
      break;
    }
  }
  if(fd < 0){
    /* 进程的文件描述符表满了 */
    fileclose(f);
    iunlock(ip);
    iput(ip);
    return -1;
  }

  /* 初始化 file 结构 */
  f->type = FD_INODE;
  f->ip = ip;                              /* 指向文件的 inode */
  f->off = 0;                              /* 偏移从 0 开始 */
  f->readable = !(omode & O_WRONLY);       /* 非只写 = 可读 */
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);  /* 只写或读写 = 可写 */

  iunlock(ip);  /* 解锁 inode（file 结构持有引用，不会被回收） */
  return fd;    /* 返回文件描述符 */
}

/* ================================================================
 * sys_read — 从文件读取数据
 *
 * 从 trapframe 读取参数：
 *   a0 = fd（文件描述符）
 *   a1 = addr（目标缓冲区的用户虚拟地址）
 *   a2 = n（要读取的字节数）
 *
 * 流程：检查 fd 合法性 → fileread 执行实际读取
 * ================================================================ */
uint64 sys_read(void) {
  int fd;
  uint64 addr;
  int n;
  struct proc *p = myproc();
  /* 提取参数 */
  if(argint(0, &fd) < 0 || argaddr(1, &addr) < 0 || argint(2, &n) < 0)
    return -1;
  /* 检查 fd 合法性 */
  if(fd < 0 || fd >= NOFILE || p->ofile[fd] == 0)
    return -1;
  /* 调用文件描述符层的 fileread */
  return fileread(p->ofile[fd], addr, n);
}

/* ================================================================
 * consolewrite — 将字节写入 UART 控制台（用于 stdout fd=1）
 *
 * 特殊处理：fd=1 是标准输出（stdout），内核启动时没有为它分配 struct file，
 * 所以直接通过 UART 驱动输出字符。
 * ================================================================ */
static int consolewrite(uint64 src, int n) {
  int i;
  char c;
  for(i = 0; i < n; i++){
    /* 恒等映射下直接读取用户虚拟地址 */
    c = *(char*)(src + i);
    uart_putc(c);  /* 输出单个字符到 UART */
  }
  return n;
}

/* ================================================================
 * sys_write — 向文件写入数据
 *
 * 从 trapframe 读取参数：
 *   a0 = fd（文件描述符）
 *   a1 = addr（源数据的用户虚拟地址）
 *   a2 = n（要写入的字节数）
 *
 * 特殊处理：fd=1（stdout）直接调用 consolewrite 输出到 UART，
 * 其他 fd 调用 filewrite 写入文件。
 * ================================================================ */
uint64 sys_write(void) {
  int fd;
  uint64 addr;
  int n;
  struct proc *p = myproc();
  /* 提取参数 */
  if(argint(0, &fd) < 0 || argaddr(1, &addr) < 0 || argint(2, &n) < 0)
    return -1;

  /* fd=1 是 stdout（控制台输出）：直接写 UART */
  if(fd == 1 && p->ofile[fd] == 0)
    return consolewrite(addr, n);

  /* 检查 fd 合法性 */
  if(fd < 0 || fd >= NOFILE || p->ofile[fd] == 0)
    return -1;
  /* 调用文件描述符层的 filewrite */
  return filewrite(p->ofile[fd], addr, n);
}

/* ================================================================
 * sys_close — 关闭文件描述符
 *
 * 流程：
 *   1. 检查 fd 合法性
 *   2. fileclose(f) — 释放 struct file（ref--，归零则释放 inode 引用）
 *   3. p->ofile[fd] = 0 — 清空 fd 槽位
 * ================================================================ */
uint64 sys_close(void) {
  int fd;
  struct proc *p = myproc();
  if(argint(0, &fd) < 0) return -1;
  if(fd < 0 || fd >= NOFILE || p->ofile[fd] == 0) return -1;
  fileclose(p->ofile[fd]);  /* 关闭文件（减少引用计数） */
  p->ofile[fd] = 0;         /* 清空进程的 fd 槽位 */
  return 0;
}

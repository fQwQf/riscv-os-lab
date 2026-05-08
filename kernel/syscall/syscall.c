/* syscall.c — 系统调用分发（Lab6 任务3）
 *
 * 【系统调用的工作原理】
 * 用户程序在 usys.S 中通过 ecall 指令陷入内核（U-Mode → S-Mode）。
 * 内核在 usertrap() 中检测到 scause==8（U-Mode ecall），
 * 调用本文件的 syscall() 函数进行分发：
 *   1. 从 trapframe 读取系统调用号（a7 寄存器的值）
 *   2. 在函数指针表 syscalls[] 中查找对应的内核实现函数
 *   3. 调用该函数，将返回值写回 trapframe 的 a0 寄存器
 *
 * 参数传递方式：
 *   用户程序将参数放入 a0/a1/a2 寄存器，
 *   内核通过 argraw(n) 从 trapframe 读取第 n 个参数的值。
 *   返回值写入 trapframe->a0，sret 返回用户态后用户程序从 a0 取结果。
 */

#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "riscv.h"
#include "types.h"

/* ---- 系统调用号常量（与 usys.S 中的定义一一对应）---- */
#define SYS_fork 1
#define SYS_exit 2
#define SYS_wait 3
#define SYS_getpid 11
#define SYS_sbrk 12
#define SYS_write 16
#define SYS_open  15
#define SYS_read  13
#define SYS_close 17

/* 获取数组长度的宏 */
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

/* ---- 各系统调用实现函数的声明（实现在 sysproc.c 和 sysfile.c）---- */
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_write(void);
extern uint64 sys_open(void);
extern uint64 sys_read(void);
extern uint64 sys_close(void);

/* 系统调用函数指针表（使用 C 的指定初始化语法）
 *
 * syscalls[SYS_open] = sys_open 表示：
 * 当 a7 寄存器 == 15 时，调用 sys_open() 函数。
 *
 * ⚠ 这里的编号必须与 usys.S 中的 li a7, SYS_xxx 完全一致！ */
static uint64 (*syscalls[])(void) = {
    [SYS_fork]   = sys_fork,
    [SYS_exit]   = sys_exit,
    [SYS_wait]   = sys_wait,
    [SYS_getpid] = sys_getpid,
    [SYS_sbrk]   = sys_sbrk,
    [SYS_write]  = sys_write,
    [SYS_open]   = sys_open,
    [SYS_read]   = sys_read,
    [SYS_close]  = sys_close,
};

/* ================================================================
 * syscall — 系统调用分发主函数（由 usertrap 调用）
 *
 * 流程：
 *   1. myproc() 获取当前进程的 proc 结构
 *   2. 从 trapframe->a7 读取系统调用号
 *   3. 在 syscalls[] 中查找并调用对应函数
 *   4. 返回值写入 trapframe->a0（用户态从 a0 取结果）
 * ================================================================ */
void syscall(void) {
  struct proc *p = myproc();
  int num = p->trapframe->a7;  /* 系统调用号（由 usys.S 的 li a7, SYS_xxx 设置） */

  if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    /* 调用对应的内核实现函数，返回值写入 a0
     * 例如 num=15 → syscalls[15]=sys_open → 调用 sys_open()
     * sys_open 的返回值（fd）会写入 trapframe->a0 */
    p->trapframe->a0 = syscalls[num]();
  } else {
    /* 无效的系统调用号 */
    printf("syscall: unknown syscall num=%d pid=%d\n", num, p->pid);
    p->trapframe->a0 = -1;  /* 返回 -1 表示错误 */
  }
}

/* ================================================================
 * 参数提取辅助函数
 *
 * 系统调用的参数不是通过 C 函数参数传递的（因为 ecall 不传参），
 * 而是存在 trapframe 的 a0-a5 寄存器中。
 * 这些函数从 trapframe 中取出对应寄存器的值。
 *
 *   argraw(n)      — 读取第 n 个参数的原始 uint64 值
 *                    n=0 → a0, n=1 → a1, n=2 → a2, ...
 *   argint(n, ip)  — 读取整数参数（通过 argraw 转为 int）
 *   argaddr(n, ap) — 读取地址参数（通过 argraw 获取 uint64）
 *   argstr(n, buf, max) — 读取字符串参数（从用户地址拷贝到内核缓冲区）
 * ================================================================ */

static uint64 argraw(int n) {
  struct proc *p = myproc();
  /* trapframe 中 a0-a5 分别对应函数参数的第 0-5 个
   * 偏移量与 proc.h 中 struct trapframe 的字段顺序一致 */
  switch (n) {
  case 0: return p->trapframe->a0;
  case 1: return p->trapframe->a1;
  case 2: return p->trapframe->a2;
  case 3: return p->trapframe->a3;
  case 4: return p->trapframe->a4;
  case 5: return p->trapframe->a5;
  default:
    panic("argraw: invalid argument index");
  }
  return 0;
}

/* 读取整数参数 */
int argint(int n, int *ip) {
  *ip = (int)argraw(n);  /* 从 trapframe 取出 uint64，转为 int */
  return 0;
}

/* 读取地址参数 */
int argaddr(int n, uint64 *ap) {
  *ap = argraw(n);  /* 直接取 uint64 值 */
  return 0;
}

/* 读取字符串参数：从用户虚拟地址拷贝到内核缓冲区
 *
 * 由于本框架使用恒等映射（用户虚拟地址 = 物理地址），
 * 可以直接通过指针访问用户空间的数据。
 * 完整版（如 xv6）需要通过 copyinstr() 安全地跨页表拷贝。 */
int argstr(int n, char *buf, int max) {
  uint64 addr;
  if (argaddr(n, &addr) < 0)
    return -1;

  /* 恒等映射下用户虚拟地址 = 物理地址，可直接访问 */
  char *src = (char *)addr;
  int i;
  for (i = 0; i < max - 1; i++) {
    buf[i] = src[i];       /* 逐字节拷贝 */
    if (src[i] == '\0')    /* 遇到字符串结尾 */
      return i;            /* 返回字符串长度（不含 '\0'） */
  }
  buf[i] = '\0';           /* 超过 max-1 则截断 */
  return i;
}

/* syscall.c — 系统调用分发（Lab6 任务3）
 *
 * 当用户程序执行 ecall 并由 usertrap() 捕获后，
 * 调用本文件的 syscall() 函数进行分发：
 *   1. 从陷阱帧读取系统调用号（a7 寄存器的值）
 *   2. 在函数指针表中查找对应的内核实现函数
 *   3. 调用该函数，将返回值写回陷阱帧的 a0 寄存器
 */

#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "riscv.h"
#include "types.h"

/* 系统调用号常量定义 */
#define SYS_fork 1
#define SYS_exit 2
#define SYS_wait 3
#define SYS_getpid 11
#define SYS_sbrk 12
#define SYS_write 16

/* 获取定义长度的宏 */
#define NELEM(x) (sizeof(x) / sizeof((x)[0]))

/* extern 声明来自 sysproc.c 的实现函数 */
extern uint64 sys_fork(void);
extern uint64 sys_exit(void);
extern uint64 sys_wait(void);
extern uint64 sys_getpid(void);
extern uint64 sys_sbrk(void);
extern uint64 sys_write(void);

/* 系统调用函数指针表（使用指定初始化语法）*/
static uint64 (*syscalls[])(void) = {
    [SYS_fork]   = sys_fork,
    [SYS_exit]   = sys_exit,
    [SYS_wait]   = sys_wait,
    [SYS_getpid] = sys_getpid,
    [SYS_sbrk]   = sys_sbrk,
    [SYS_write]  = sys_write,
};

/* ================================================================
 * syscall — 系统调用分发主函数（由 usertrap 调用）
 * ================================================================ */
void syscall(void) {
  struct proc *p = myproc();
  int num = p->trapframe->a7;

  if (num > 0 && num < NELEM(syscalls) && syscalls[num]) {
    p->trapframe->a0 = syscalls[num]();
  } else {
    printf("syscall: unknown syscall num=%d pid=%d\n", num, p->pid);
    p->trapframe->a0 = -1;
  }
}

/* ================================================================
 * 参数提取辅助函数（Lab6 任务4）
 *
 * argraw(n)    — 从 trapframe 读取第 n 个参数的原始值
 * argint(n,ip) — 读取整数参数
 * argaddr(n,ap)— 读取地址参数
 * argstr(n,buf,max) — 安全拷贝字符串参数
 * ================================================================ */

static uint64 argraw(int n) {
  struct proc *p = myproc();
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

int argint(int n, int *ip) {
  *ip = (int)argraw(n);
  return 0;
}

int argaddr(int n, uint64 *ap) {
  *ap = argraw(n);
  return 0;
}

int argstr(int n, char *buf, int max) {
  uint64 addr;
  if (argaddr(n, &addr) < 0)
    return -1;

  /* 恒等映射下用户虚拟地址 = 物理地址，可直接访问 */
  char *src = (char *)addr;
  int i;
  for (i = 0; i < max - 1; i++) {
    buf[i] = src[i];
    if (src[i] == '\0')
      return i;
  }
  buf[i] = '\0';
  return i;
}

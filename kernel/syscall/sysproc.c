/* sysproc.c — 系统调用内核实现（Lab6 任务5、任务7）
 *
 * 每个 sys_xxx() 函数是对应系统调用的真正内核实现。
 * 它们不接受参数（参数通过陷阱帧的寄存器传入，用 argint/argaddr 读取），
 * 返回 uint64 类型的结果值。
 */

#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "proc.h"
#include "riscv.h"
#include "types.h"

uint64 sys_getpid(void) {
  return myproc()->pid;
}

uint64 sys_exit(void) {
  int n;
  argint(0, &n);
  exit(n);
  return 0;
}



uint64 sys_fork(void) {
  return fork();
}

uint64 sys_wait(void) {
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64 sys_sbrk(void) {
  return -1;
}

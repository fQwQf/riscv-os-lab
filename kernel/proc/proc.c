/* proc.c — 进程管理（Lab5 任务1、3、4）
 *
 * 本文件实现了进程生命周期管理的核心逻辑：
 *   - procinit()   : 初始化进程表
 *   - allocproc()  : 为新进程分配 PCB
 *   - scheduler()  : 调度器主循环（无限轮询、找到就绪进程就运行）
 *   - yield()      : 当前进程主动放弃 CPU（配合时钟中断使用）
 *   - userinit()   : 创建第一个用户态进程 proczero（Lab5 任务B）
 *   - forkret()    : 进程首次被调度时的"出生之地"（Lab5 任务B）
 */

#include "proc.h"
#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "riscv.h"
#include "types.h"

/* 全局进程表和 CPU 描述符（在 proc.h 中 extern 声明）*/
struct proc proc[NPROC];
struct cpu cpus[NCPU];

/* 进程 ID 计数器（每次 allocpid 返回后递增）*/
static int nextpid = 1;

/* 内核页表（定义在 vm.c 中）*/
extern pagetable_t kernel_pagetable;

/* proczero 用户程序机器码（由 user/proczero.c 编译生成）*/
#include "usercode.h"

/* 前向声明 */
void forkret(void);

/* ================================================================
 * mycpu — 获取当前 CPU 核心的 cpu 结构指针
 *
 * 实现方式：读取 tp 寄存器（在 start.c 中被设置为 hartid）
 * ================================================================ */
struct cpu *mycpu(void) {
  int hartid = r_tp();
  return &cpus[hartid];
}

/* ================================================================
 * myproc — 获取当前 CPU 上正在运行的进程的 PCB 指针
 * ================================================================ */
struct proc *myproc(void) { return mycpu()->proc; }

/* ================================================================
 * allocpid — 分配一个唯一的进程 ID
 * ================================================================ */
int allocpid(void) { return nextpid++; }

/* ================================================================
 * procinit — 初始化进程表（内核启动时调用一次）
 *
 * 任务：将进程表中所有条目的状态初始化为 TASK_FREE。
 * ================================================================ */
void procinit(void) {
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    p->status = TASK_FREE;
  }
}

/* ================================================================
 * allocproc — 在进程表中找一个空槽并初始化
 *
 * 返回：指向已初始化的 PCB 的指针；若进程表满，返回 0。
 *
 * 初始化内容：
 *   - 分配 pid
 *   - 将状态从 TASK_FREE 改为 TASK_ALLOCATED
 *   - 分配 trapframe 页（用于保存用户寄存器）
 *   - 初始化内核 context（ra 设为 forkret）
 * ================================================================ */
struct proc *allocproc(void) {
  struct proc *p;

  /* 在进程表中寻找一个 TASK_FREE 的槽位 */
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p->status == TASK_FREE)
      goto found;
  }
  return 0; /* 进程表已满 */

found:
  /* 1. 分配 pid */
  p->pid = allocpid();

  /* 2. 分配 trapframe 页 */
  p->trapframe = (struct trapframe *)kalloc();
  if (p->trapframe == 0) {
    p->status = TASK_FREE;
    return 0;
  }

  /* 3. 将进程状态设为 TASK_ALLOCATED */
  p->status = TASK_ALLOCATED;

  /* 初始化 Lab6 新增字段 */
  p->parent = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;

  /* 4. 初始化 context：清零后设置 ra = forkret */
  /* 清零 context */
  uint64 *ctx = (uint64 *)&p->context;
  for (int i = 0; i < sizeof(struct context) / 8; i++)
    ctx[i] = 0;

  /* ra 设为 forkret，使得第一次 swtch 返回时跳到 forkret */
  p->context.ra = (uint64)forkret;

  return p;
}

/* ================================================================
 * forkret — 进程首次被调度时的"出生之地"（Task B）
 *
 * 当 scheduler() 通过 swtch() 第一次切换到新进程时，
 * CPU 执行的第一个函数就是 forkret()。
 *
 * 它的职责：调用 usertrapret() 完成从 S-Mode 到 U-Mode 的切换。
 *
 * 为什么需要 forkret 而不是直接设置 context.ra = usertrapret？
 * 因为 scheduler() 在调用 swtch() 之前可能持有某些资源（如锁），
 * forkret 可以在此处释放这些资源，确保安全地进入用户态。
 * ================================================================ */
void forkret(void) {
  /* 调用 usertrapret() 完成 S→U 的最后一步 */
  usertrapret();
}

/* ================================================================
 * userinit — 创建第一个用户态进程 proczero（Task B）
 *
 * 这是系统启动时创建的第一个进程。它的用户代码是
 * proczero_code[] 字节数组中嵌入的机器码。
 * ================================================================ */
void userinit(void) {
  struct proc *p;

  /* 1. 调用 allocproc() 获取已初始化基础字段的 PCB */
  p = allocproc();
  if (p == 0)
    panic("userinit: allocproc failed");

  /* 2. 分配内核栈 */
  p->kstack = (uint64)kalloc();
  if (p->kstack == 0)
    panic("userinit: kalloc kstack failed");
  /* 栈从高地址向低地址生长，sp 初始化为栈顶（高地址端） */
  p->context.sp = p->kstack + PGSIZE;

  /* 3. 为用户代码分配物理内存，拷贝 proczero_code，建立页表映射 */
  void *user_code_page = kalloc();
  if (user_code_page == 0)
    panic("userinit: kalloc user code failed");

  /* 拷贝用户代码到新分配的页 */
  uint8 *dst = (uint8 *)user_code_page;
  for (int i = 0; i < (int)sizeof(proczero_code); i++)
    dst[i] = proczero_code[i];

  /* 这些页已经被 kvmininit() 恒等映射过了（PTE_R|PTE_W），
   * 不能再次 mappages（会触发 remap panic）。
   * 改为用 walk() 找到已有的 PTE，追加 PTE_U 和 PTE_X 权限位。 */
  pte_t *pte = walk(kernel_pagetable, (uint64)user_code_page, 0);
  if (pte == 0)
    panic("userinit: walk user code failed");
  *pte |= PTE_X | PTE_U;

  /* 4. 为用户栈分配物理内存，添加 PTE_U 权限 */
  void *user_stack_page = kalloc();
  if (user_stack_page == 0)
    panic("userinit: kalloc user stack failed");

  pte = walk(kernel_pagetable, (uint64)user_stack_page, 0);
  if (pte == 0)
    panic("userinit: walk user stack failed");
  *pte |= PTE_U;

  /* 5. 初始化 trapframe */
  /* 清零 trapframe */
  uint64 *tf = (uint64 *)p->trapframe;
  for (int i = 0; i < PGSIZE / 8; i++)
    tf[i] = 0;

  /* epc = 用户代码入口地址（恒等映射，物理地址 = 虚拟地址） */
  p->trapframe->epc = (uint64)user_code_page;
  /* sp = 用户栈顶（栈页的最高地址） */
  p->trapframe->sp = (uint64)user_stack_page + PGSIZE;

  /* 6. 设置进程元信息 */
  p->pagetable = kernel_pagetable;
  p->sz = PGSIZE * 2; /* 用户代码页 + 用户栈页 */

  /* 设置进程名称 */
  char *name = "proczero";
  int i;
  for (i = 0; name[i] && i < 15; i++)
    p->name[i] = name[i];
  p->name[i] = 0;

  /* proczero 是 init 进程，没有父进程 */
  p->parent = 0;

  /* 将状态设为 TASK_READY，允许调度器选中 */
  p->status = TASK_READY;

  printf("userinit: proczero created (pid=%d, epc=%p, sp=%p)\n",
         p->pid, p->trapframe->epc, p->trapframe->sp);
}

/* ================================================================
 * scheduler — 调度器主循环（永不返回！）
 *
 * 这是操作系统的"上帝"：它在所有进程之间无限轮转，
 * 当看到一个 TASK_READY 的进程时，就把 CPU 交给它。
 * ================================================================ */
void scheduler(void) {
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;

  for (;;) {
    /* 必须打开中断！否则时钟信号无法到达，调度无法触发 */
    intr_on();

    for (p = proc; p < &proc[NPROC]; p++) {
      if (p->status == TASK_READY) {
        /* 将状态改为 TASK_RUNNING */
        p->status = TASK_RUNNING;
        /* 设置当前 CPU 运行的进程 */
        c->proc = p;
        /* 切换到进程的内核上下文 */
        swtch(&c->context, &p->context);
        /* swtch 返回后（进程放弃了CPU），清零 c->proc */
        c->proc = 0;
      }
    }
  }
}

/* ================================================================
 * yield — 当前进程主动放弃 CPU（由时钟中断处理函数调用）
 *
 * 过程：将自己的状态从 TASK_RUNNING 改回 TASK_READY，然后切回调度器。
 * ================================================================ */
void yield(void) {
  struct proc *p = myproc();

  /* 将进程状态改为 TASK_READY */
  p->status = TASK_READY;
  /* 切回调度器上下文 */
  swtch(&p->context, &mycpu()->context);
}

/* ================================================================
 * sleep — 将当前进程挂起，等待被 wakeup 唤醒
 * ================================================================ */
void sleep(void *chan) {
  struct proc *p = myproc();
  p->chan = chan;
  p->status = TASK_SLEEPING;
  swtch(&p->context, &mycpu()->context);
  p->chan = 0;
}

/* ================================================================
 * wakeup — 唤醒所有在 chan 上等待的进程
 * ================================================================ */
void wakeup(void *chan) {
  struct proc *p;
  for (p = proc; p < &proc[NPROC]; p++) {
    if (p != myproc() && p->status == TASK_SLEEPING && p->chan == chan) {
      p->status = TASK_READY;
    }
  }
}

/* ================================================================
 * exit — 终止当前进程
 *
 * 1. 保存退出码
 * 2. 将子进程移交 init（pid=1）
 * 3. 唤醒父进程
 * 4. 变为 ZOMBIE，切回调度器（永不返回）
 * ================================================================ */
void exit(int status) {
  struct proc *p = myproc();

  p->xstate = status;

  printf("Process %d exited with status %d\n", p->pid, status);

  /* 将子进程移交给 init (pid=1) */
  struct proc *pp;
  for (pp = proc; pp < &proc[NPROC]; pp++) {
    if (pp->parent == p) {
      pp->parent = &proc[0];
    }
  }

  wakeup(p->parent);

  p->status = TASK_ZOMBIE;
  swtch(&p->context, &mycpu()->context);

  panic("exit: zombie returned");
}

/* ================================================================
 * wait — 等待子进程退出，回收其资源
 *
 * addr: 用户空间地址，用于写入子进程退出码（0 表示不需要）
 * 返回: 子进程 pid，或 -1（没有子进程）
 * ================================================================ */
int wait(uint64 addr) {
  struct proc *p = myproc();
  struct proc *child;
  int havekids, pid;

  for (;;) {
    havekids = 0;
    for (child = proc; child < &proc[NPROC]; child++) {
      if (child->parent == p) {
        havekids = 1;
        if (child->status == TASK_ZOMBIE) {
          pid = child->pid;
          if (addr != 0) {
            /* 恒等映射下直接写入用户地址 */
            *(int *)addr = child->xstate;
          }
          /* 释放子进程资源 */
          child->status = TASK_FREE;
          child->pid = 0;
          child->parent = 0;
          child->name[0] = 0;
          if (child->trapframe) {
            kfree(child->trapframe);
            child->trapframe = 0;
          }
          if (child->kstack) {
            kfree((void *)child->kstack);
            child->kstack = 0;
          }
          return pid;
        }
      }
    }

    if (!havekids)
      return -1;

    sleep(p);
  }
}

/* ================================================================
 * fork — 创建子进程（复制当前进程）
 *
 * 返回: 父进程得到子进程 pid，子进程得到 0
 * ================================================================ */
int fork(void) {
  struct proc *p = myproc();

  struct proc *np = allocproc();
  if (np == 0)
    return -1;

  /* 分配内核栈 */
  np->kstack = (uint64)kalloc();
  if (np->kstack == 0) {
    np->status = TASK_FREE;
    return -1;
  }
  np->context.sp = np->kstack + PGSIZE;

  /* 复制 trapframe */
  uint64 *dst = (uint64 *)np->trapframe;
  uint64 *src = (uint64 *)p->trapframe;
  for (int i = 0; i < PGSIZE / 8; i++)
    dst[i] = src[i];

  /* 子进程从 fork 返回 0 */
  np->trapframe->a0 = 0;

  /* 共享内核页表（恒等映射下父子进程共享物理页面）*/
  np->pagetable = p->pagetable;
  np->sz = p->sz;

  /* 建立父子关系 */
  np->parent = p;

  /* 复制进程名 */
  for (int i = 0; i < 16; i++)
    np->name[i] = p->name[i];

  /* 子进程就绪 */
  np->status = TASK_READY;

  return np->pid;
}

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

/* proczero 用户态程序的机器码（Task D）：
 *   ecall           → 0x00000073  第一次系统调用
 *   ecall           → 0x00000073  第二次系统调用
 *   j .  (死循环)   → 0x0000006f
 */
static uint8 proczero_code[] = {
  0x73, 0x00, 0x00, 0x00, // ecall
  0x73, 0x00, 0x00, 0x00, // ecall
  0x6f, 0x00, 0x00, 0x00, // j .
};

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

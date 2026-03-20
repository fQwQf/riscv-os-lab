/* trap.c — 中断与异常分发（Lab4 任务1&3，Lab6 扩展）
 *
 * 本文件是内核的"中控室"。当 sys_trap_vector 把寄存器保存完毕，
 * 就会调用 sys_trap_handler()，由它来判断发生了什么事并分派处理。
 *
 * Lab4 实现：处理时冲中断，每次打印 "Tick!"
 * Lab5 扩展：在时钟中断中增加 yield()，触发进程调度
 * Lab6 扩展：增加 usertrap()，处理来自用户态的 ecall 系统调用
 */

#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "riscv.h"
#include "types.h"

/* 声明 sys_trap_vector 汇编入口（在 kernelvec.S 中定义）*/
extern char sys_trap_vector[];

/* ================================================================
 * trapinithart — 设置 S-Mode 陷阱向量
 *
 * 告诉 CPU：当 S-Mode 下发生中断/异常时，跳转到 sys_trap_vector。
 * 在 main.c 的 start_main() 中调用一次即可（每个 CPU 核心调用一次）。
 * ================================================================ */
void trapinithart(void) {
  /* ================================================================
   * TODO [Lab4-任务1]：
   *   将 sys_trap_vector 的地址写入 stvec 寄存器。
   *   使用 w_stvec() 函数（已在 riscv.h 中定义）。
   * ================================================================ */
}

/* ================================================================
 * sys_trap_handler — 内核态中断/异常总处理函数（由 sys_trap_vector 汇编调用）
 *
 * 本函数从 CSR 读取中断原因，然后根据类型分发处理：
 *
 *   scause 最高位（bit 63）：
 *     = 1 → 异步中断（Interrupt），低位表示具体类型
 *     = 0 → 同步异常（Exception），不应在内核中发生
 *
 *   常见中断类型（irq 值）：
 *     1  → 软件中断（由 M-Mode 的 timervec 注入的时钟信号）
 *     5  → S-Mode 时钟中断（如果直接委托到 S-Mode）
 *     9  → 外部中断（UART 键盘输入等）
 * ================================================================ */
void sys_trap_handler(void) {
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();

  /* 验证：进入内核陷阱前，S-Mode 的中断应该已经关闭 */
  if ((sstatus & SSTATUS_SPP) == 0)
    panic("sys_trap_handler: not from supervisor mode");
  if (intr_get())
    panic("sys_trap_handler: entered with interrupts enabled");

  if (scause & 0x8000000000000000L) {
    /* 这是一个异步中断 */
    uint64 irq = scause & 0xff;

    switch (irq) {
    case 1:
      /* ================================================================
       * TODO [Lab4-任务3-步骤1]：
       *   S-Mode 软件中断（由 M-Mode timervec 触发的时钟信号）。
       *
       *   处理步骤：
       *   1. 清除 sip 中的 SSIP 位（否则中断会持续触发）：接口为 w_sip(r_sip() & ~2)
       *   2. 打印 "Tick!\n"（验收用）
       *   3. （Lab5完成后追加）若有运行中的进程，调用 yield() 切换
       * ================================================================ */
      break;

    case 9:
      /* 外部中断（如 UART 键盘）：Lab7 之前可暂不处理 */
      break;

    default:
      printf("sys_trap_handler: unknown interrupt irq=%ld\n", irq);
      break;
    }

  } else {
    /* 同步异常：内核代码出了错，无法恢复，直接 panic */
    printf("sys_trap_handler: exception! scause=%ld, sepc=%p, stval=%p\n",
           scause, sepc, r_stval());
    panic("sys_trap_handler: unexpected exception");
  }

  /* ================================================================
   * 恢复 sepc 和 sstatus：
   * 某些情况下（如嵌套中断）它们可能被修改过，需要还原。
   * ================================================================ */
  w_sepc(sepc);
  w_sstatus(sstatus);
}

/* ================================================================
 * usertrap — 用户态陷阱处理（Lab6 新增）
 *
 * 当用户程序执行 ecall 时，CPU 切换到 S-Mode 并调用此函数。
 *
 * 区别于 sys_trap_handler：
 *   - 需要切换陷阱向量到 sys_trap_vector（防止用户态 PC 出现在栈跟踪里）
 *   - 需要将 epc 加 4，跳过 ecall 指令（否则返回后又会执行 ecall）
 *   - 只处理 scause == 8（来自 U-Mode 的 ecall）
 * ================================================================ */
void usertrap(void) {
  /* 立即切换到内核态陷阱向量（防止处理用户陷阱时再发生用户态中断）*/
  w_stvec((uint64)sys_trap_vector);

  uint64 cause = r_scause();

  if (cause == 8) {
    /* 来自 U-Mode 的 ecall（系统调用）*/

    /* 允许中断（系统调用可能涉及耗时 I/O 操作）*/
    intr_on();

    /* ================================================================
     * TODO [Lab6-任务2]：
     *   将被打断的 PC（sepc）向后移动 4 字节，跳过 ecall 指令。
     *   需要通过 myproc()->trapframe->epc 访问该字段并对其加 4。
     *   如不执行此步，返回后用户态会无限重复执行 ecall！
     * ================================================================ */

    /* 分发给系统调用处理函数 */
    syscall();

  } else {
    /* 用户态发生异常（如非法内存访问），直接终止该进程 */
    printf("usertrap: unexpected scause=%ld\n", cause);
    /* 理想情况下应该 exit(-1) 杀死该进程，暂不实现 */
    panic("usertrap");
  }
}

/* main.c — 内核 C 语言主函数（Lab1 任务4，后续实验持续扩展）
 *
 * 注意：随着实验推进，你需要在 start_main() 中依次添加新模块的初始化调用。
 * 每个实验结束后，start_main() 大约会是什么样子，注释中有说明。
 */

/* =============================================================
 * Lab1 完成后，这个文件应该像这样：
 *
 *   extern void uart_puts(char *s);
 *   void start_main() {
 *       uart_puts("Hello OS from RISC-V Bare-metal!\n");
 *       while(1);
 *   }
 *
 * Lab2 完成后，扩展为调用 printf 和 clear_screen。
 * Lab3 完成后，增加 kinit(), kvmininit(), kvminithart()。
 * Lab4 完成后，增加 trapinithart(), start()（移至 start.c）。
 * Lab5 完成后，增加 procinit(), scheduler()。
 * ============================================================= */

#include "defs.h"
#include "riscv.h"

void start_main() {
  clear_screen();
  printf("Welcome to WHU OS Lab!\n");

  /* ================================================================
   * Lab3 新增：初始化物理内存与虚拟内存
   * ================================================================ */
  kinit();            /* 1. 初始化物理内存分配器 */
  kvmininit();        /* 2. 建立内核页表（映射内存与外设）*/
  kvminithart();      /* 3. 开启分页（将页表地址写入 satp 寄存器）*/

  /* ================================================================
   * Lab4 新增：注册陷阱向量并开启中断
   * ================================================================ */
  trapinithart();     /* 4. 注册 S-Mode 陷阱向量 */
  uartinit();         /* 5. 初始化 UART 硬件（使能接收中断）*/
  plicinit();         /* 6. 配置 PLIC 中断优先级（全局）*/
  plicinithart();     /* 7. 配置本核心的 PLIC 使能和阈值 */
  intr_on();          /* 8. 开启 S-Mode 全局中断 */

  clear_screen();
  printf("Paging enabled! Interrupts on.\n");

  while (1)
    ; /* 内核死循环，不要删除 */
}


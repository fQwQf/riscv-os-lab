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

void start_main() {
  clear_screen(); // 验收：屏幕瞬间变干净！
  printf("Welcome to WHU OS Lab!\n");
  // 验收：这三个特殊的占位符必须被正确解析和打印！
  printf("Kernel loaded at address: %p\n", 0x80000000);
  printf("Signed integer test: %d\n", -123);
  printf("String test: %s\n", "Hello, Variadic Parameters!");

  while (1)
    ; /* 内核死循环，不要删除 */
}

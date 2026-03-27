/* uart.c — UART 串口驱动（Lab1 任务3）
 *
 * UART（Universal Asynchronous Receiver-Transmitter）是 QEMU 模拟的串口设备。
 * 通过"内存映射I/O (MMIO)"机制，向固定物理地址写数据 = 向终端输出字符。
 *
 * QEMU virt 平台的 UART0 基地址：0x10000000
 * 此地址已在 memlayout.h 中定义为 UART0。
 */

/* ================================================================
 * TODO [Lab1-任务3-拓展]：
 *   如果你想让 uart_putc 更稳定（等待UART真正准备好再发送），
 *   可以在写数据前检查"线状态寄存器 LSR"的第5位（发送保持寄存器空为1）。
 *   现在我们先用最简单的版本，直接写入即可正常工作。
 * ================================================================ */

/* 通过指针操作访问 UART 的 MMIO 寄存器
 * volatile 关键字告诉编译器：这个内存访问有"副作用"，不能被优化掉！
 *
 * 思考：如果去掉 volatile，用高优化级别(-O2)编译时，编译器可能认为
 * "你反复写同一地址的内存，最后一次写就够了"，会把中间的写操作删除，
 * 从而导致只有最后一个字符被发送！
 */
#define UART0_BASE 0x10000000L
#define Reg(offset) ((volatile unsigned char *)(UART0_BASE + (offset)))

/* 16550 UART 寄存器偏移 */
#define RHR 0   /* Receive Holding Register (read) */
#define THR 0   /* Transmit Holding Register (write) */
#define IER 1   /* Interrupt Enable Register */
#define FCR 2   /* FIFO Control Register (write) */
#define LCR 3   /* Line Control Register */
#define MCR 4   /* Modem Control Register */
#define LSR 5   /* Line Status Register */

#define IER_RX_ENABLE (1 << 0)  /* 接收数据中断使能 */
#define IER_TX_ENABLE (1 << 1)  /* 发送完成中断使能 */
#define LSR_RX_READY  (1 << 0)  /* 接收数据就绪 */

/* 初始化 UART 硬件（Lab4 进阶：使能接收中断）*/
void uartinit(void) {
  /* 关闭中断 */
  *Reg(IER) = 0x00;

  /* 设置波特率：进入 DLAB 模式，写除数锁存器 */
  *Reg(LCR) = 0x80;    /* DLAB=1，开启除数锁存器访问 */
  *Reg(0) = 0x03;      /* 除数低 8 位（38400 baud） */
  *Reg(1) = 0x00;      /* 除数高 8 位 */

  /* 设置数据格式：8 数据位，无校验，1 停止位 */
  *Reg(LCR) = 0x03;    /* DLAB=0，恢复正常模式，8-N-1 */

  /* 启用 FIFO，清空收发缓冲区 */
  *Reg(FCR) = 0x07;

  /* 使能 modem 控制（DTR + RTS + OUT2，OUT2 用于使能中断输出） */
  *Reg(MCR) = 0x0B;

  /* 开启接收中断：每当 RHR 有数据时，UART 产生 IRQ */
  *Reg(IER) = IER_RX_ENABLE;
}

/* 发送一个字符到 UART（即：在终端打印一个字符）*/
void uart_putc(char c) {
  /* 等待发送保持寄存器空闲 */
  while ((*Reg(LSR) & (1 << 5)) == 0)
    ;
  *Reg(THR) = c;
}

/* 发送一个字符串到 UART（逐字符调用 uart_putc）*/
void uart_puts(char *s) {
  /* ================================================================
   * TODO [Lab1-任务3-步骤2]：
   *   遍历字符串 s，对每个字符调用 uart_putc，
   *   直到遇到字符串结束符 '\0' 为止。
   *
   *   提示：C 语言字符串始终以 '\0'（值为0的字节）结尾。
   *   while 循环条件可以写：while (*s) { ... s++; }
   * ================================================================ */

  while (*s) {
    uart_putc(*s);
    s++;
  }
}

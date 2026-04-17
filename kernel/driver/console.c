/* console.c — 内核 printf 与清屏实现（Lab2 新增文件）
 *
 * 本文件实现一个不依赖任何标准库的极简 printf，
 * 支持格式符：%d（有符号十进制）、%x（十六进制）、%p（指针）、%s（字符串）、%c（字符）。
 */

#include <stdarg.h> /* 这是编译器内置的头文件，裸机环境也可用 */

/* 声明底层单字符输出函数（在 uart.c 中实现）*/
extern void uart_putc(char c);

/* ================================================================
 * TODO [Lab2-任务1-步骤1]：
 *   实现数字到字符的映射表。
 *   它让我们可以用 digits[10] 得到 'a'，digits[15] 得到 'f'。
 * ================================================================ */
static char digits[] = "0123456789abcdef";

/* ================================================================
 * TODO [Lab2-任务1-步骤2]：
 *   实现 printint(int xx, int base, int sign) 函数。
 *
 *   功能：将整数 xx 按照 base 进制转换为字符序列并输出。
 *   参数：
 *     xx   — 要打印的整数
 *     base — 进制（10=十进制，16=十六进制）
 *     sign — 1 表示有符号数（处理负数取反），0 表示无符号数
 *
 *   算法思路（以 123 为例，base=10）：
 *     123 % 10 = 3  → buf[0] = '3'
 *     123 / 10 = 12
 *      12 % 10 = 2  → buf[1] = '2'
 *      12 / 10 = 1
 *       1 % 10 = 1  → buf[2] = '1'
 *       1 / 10 = 0  → 停止
 *     此时 buf 里的字符是倒序的！要从后往前输出。
 * ================================================================ */
static void printint(int xx, int base, int sign) {
  char buf[16];
  int i = 0;
  unsigned int x;

  if (sign && xx < 0) {
    x = (unsigned int)(-xx);
    sign = 1;
  } else {
    x = (unsigned int)xx;
    sign = 0;
  }

  do {
    buf[i++] = digits[x % base];
    x /= base;
  } while (x != 0);

  if (sign)
    buf[i++] = '-';

  while (--i >= 0)
    uart_putc(buf[i]);
}

static void printlong(long xx, int base, int sign) {
  char buf[20];
  int i = 0;
  unsigned long x;

  if (sign && xx < 0) {
    x = (unsigned long)(-xx);
    sign = 1;
  } else {
    x = (unsigned long)xx;
    sign = 0;
  }

  do {
    buf[i++] = digits[x % base];
    x /= base;
  } while (x != 0);

  if (sign)
    buf[i++] = '-';

  while (--i >= 0)
    uart_putc(buf[i]);
}

/* 单字符输出包装（方便后续扩展，比如同时写入日志缓冲区）*/
static void consputc(int c) { uart_putc((char)c); }

/* ================================================================
 * TODO [Lab2-任务2]：
 *   实现 printf(char *fmt, ...) 函数。
 *
 *   核心逻辑：
 *   - 遍历格式字符串 fmt
 *   - 遇到普通字符，直接 consputc 输出
 *   - 遇到 '%'，读取下一个字符判断格式符：
 *       %d → 取 int 参数，调用 printint(va_arg(ap, int), 10, 1)
 *       %x → 取 int 参数，调用 printint(va_arg(ap, int), 16, 0)
 *       %p → 取 uint64 参数，调用 printint(va_arg(ap, uint64), 16, 0)
 *       %s → 取 char* 参数，逐字符 consputc 输出
 *       %c → 取 int 参数，consputc 输出
 *       %% → 直接输出 '%'
 * ================================================================ */
void printf(char *fmt, ...) {
  va_list ap;
  int i, c;
  char *s;

  if (fmt == 0)
    return;

  va_start(ap, fmt);

  for (i = 0; (c = fmt[i] & 0xff) != 0; i++) {
    if (c != '%') {
      consputc(c);
      continue;
    }

    /* 遇到 '%'，读取格式符 */
    c = fmt[++i] & 0xff;
    if (c == 0)
      break;

    switch (c) {
    case 'd':
      printint(va_arg(ap, int), 10, 1);
      break;

    case 'x':
      printint(va_arg(ap, int), 16, 0);
      break;

    case 'p':
      printlong(va_arg(ap, unsigned long), 16, 0);
      break;

    case 's':
      if ((s = va_arg(ap, char *)) == 0)
        s = "(null)";
      for (; *s; s++)
        consputc(*s);
      break;

    case 'c':
      consputc(va_arg(ap, int));
      break;

    case '%':
      consputc('%');
      break;

    case 'l':
      c = fmt[++i] & 0xff;
      if (c == 'd') {
        printlong(va_arg(ap, long), 10, 1);
      } else if (c == 'u') {
        printlong(va_arg(ap, unsigned long), 10, 0);
      } else if (c == 'x') {
        printlong(va_arg(ap, unsigned long), 16, 0);
      } else {
        consputc('%');
        consputc('l');
        if (c) consputc(c);
      }
      break;

    default:
      consputc('%');
      consputc(c);
      break;
    }
  }

  va_end(ap);
}

/* ================================================================
 * TODO [Lab2-任务3]：
 *   实现 clear_screen() 函数。
 *
 *   ANSI 转义序列：
 *     "\x1b[2J" — 清除整个屏幕内容
 *     "\x1b[H"  — 将光标移动到左上角 (0,0) 位置
 *
 *   提示：现在你已经有 printf 了，可以直接用：
 *     printf("\x1b[2J");
 *     printf("\x1b[H");
 * ================================================================ */
void clear_screen(void) {
  printf("\x1b[2J");
  printf("\x1b[H");
}

/* panic — 内核致命错误处理（已提供，无需修改）
 *
 * 当内核遇到无法恢复的错误时，打印出错信息并进入死循环。
 * __attribute__((noreturn)) 告诉编译器这个函数永远不会返回。
 */
void panic(char *msg) {
  printf("\n\n");
  printf("!!! KERNEL PANIC !!!\n");
  printf("Reason: %s\n", msg);
  printf("System halted.\n");
  while (1)
    ; /* 死循环，防止CPU继续乱跑 */
}

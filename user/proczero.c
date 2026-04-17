/* proczero.c — Lab6 用户态测试程序
 *
 * 没有任何标准库，所有输出通过 write() 系统调用完成。
 */

/* 系统调用声明（实现在 usys.S）*/
int getpid(void);
int fork(void);
int wait(int *);
void exit(int);
int write(int, const void *, int);

static void printchar(char c) {
  write(1, &c, 1);
}

static void printstr(const char *s) {
  int n = 0;
  while (s[n])
    n++;
  write(1, s, n);
}

static void printnum(int n) {
  char buf[16];
  int i = 0;
  unsigned int x;

  if (n < 0) {
    printchar('-');
    x = -n;
  } else {
    x = n;
  }

  if (x == 0) {
    printchar('0');
    return;
  }

  while (x > 0) {
    buf[i++] = '0' + (x % 10);
    x /= 10;
  }
  while (--i >= 0)
    write(1, &buf[i], 1);
}

void main(void) {
  int pid = getpid();
  printstr("My pid: ");
  printnum(pid);
  printchar('\n');

  int child = fork();
  if (child == 0) {
    printstr("Child: hello\n");
    exit(1);
  } else {
    int status;
    int cpid = wait(&status);
    printstr("Parent: child ");
    printnum(cpid);
    printstr(" exited with ");
    printnum(status);
    printchar('\n');
  }

  exit(0);
}

/* 入口点 — 放在 .text.init 段，链接器脚本保证它排在最前面 */
void _start(void) __attribute__((section(".text.init")));
void _start(void) {
  main();
  exit(0);
}

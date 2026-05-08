/* proczero.c — Lab6/Lab7 用户态测试程序
 *
 * 这是内核启动后第一个运行的用户程序（由 userinit 创建）。
 * 没有任何标准库，所有输出通过 write() 系统调用完成。
 *
 * 本程序测试两个功能：
 *   1. Lab6：getpid() 系统调用、fork/wait/exit
 *   2. Lab7：open/write/read/close 文件系统操作
 */

/* 系统调用声明（实现在 usys.S 中的汇编桩） */
int getpid(void);
int fork(void);
int wait(int *);
void exit(int);
int write(int, const void *, int);
int open(const char *, int);
int read(int, void *, int);
int close(int);

/* 文件打开标志（与内核 sysfile.c 中的定义一致） */
#define O_RDONLY 0x000   /* 只读 */
#define O_WRONLY 0x001   /* 只写 */
#define O_RDWR   0x002   /* 读写 */
#define O_CREAT  0x200   /* 若文件不存在则创建 */

/* ---- 简易输出函数（无标准库，手动实现）---- */

/* 输出单个字符到 stdout（fd=1） */
static void printchar(char c) {
  write(1, &c, 1);
}

/* 输出字符串到 stdout */
static void printstr(const char *s) {
  int n = 0;
  while (s[n])     /* 计算字符串长度 */
    n++;
  write(1, s, n);  /* 通过 write 系统调用输出 */
}

/* 输出整数到 stdout（十进制） */
static void printnum(int n) {
  char buf[16];
  int i = 0;
  unsigned int x;

  if (n < 0) {
    printchar('-');   /* 处理负数 */
    x = -n;
  } else {
    x = n;
  }

  if (x == 0) {
    printchar('0');
    return;
  }

  /* 从低位到高位逐位提取 */
  while (x > 0) {
    buf[i++] = '0' + (x % 10);
    x /= 10;
  }
  /* 从高位到低位输出（逆序） */
  while (--i >= 0)
    write(1, &buf[i], 1);
}

/* ================================================================
 * main — 用户程序主函数
 * ================================================================ */
void main(void) {
  /* ---- Lab6 测试：getpid ---- */
  int pid = getpid();
  printstr("My pid: ");
  printnum(pid);
  printchar('\n');

  /* ---- Lab7 测试1：创建文件并写入数据 ---- */
  printstr("Test 1: Writing file /hello.txt\n");

  /* open("/hello.txt", O_CREAT | O_WRONLY)
   * → ecall → sys_open → nameiparent → ialloc → dirlink → filealloc → 返回 fd */
  int fd = open("/hello.txt", O_CREAT | O_WRONLY);
  if(fd < 0) {
    printstr("open failed\n");
  } else {
    /* write(fd, "Hello, File System!\n", 20)
     * → ecall → sys_write → filewrite → writei → bmap → balloc → bread → bwrite */
    int r = write(fd, "Hello, File System!\n", 20);
    if(r != 20) {
      printstr("write failed\n");
    } else {
      printstr("Test 1 passed: write 20 bytes\n");
    }
    /* close(fd)
     * → ecall → sys_close → fileclose → iput（纯内存操作，无磁盘I/O） */
    close(fd);
  }

  /* ---- Lab7 测试2：读回文件内容 ---- */
  printstr("Test 2: Reading file /hello.txt\n");

  /* open("/hello.txt", O_RDONLY) — 不带 O_CREAT，只查找已有文件 */
  fd = open("/hello.txt", O_RDONLY);
  if(fd < 0) {
    printstr("reopen failed\n");
  } else {
    char buf[64];
    /* 清零 buf */
    for(int i=0; i<64; i++) buf[i] = 0;

    /* read(fd, buf, 64)
     * → ecall → sys_read → fileread → readi → bmap → bread（可能缓存命中） */
    int r = read(fd, buf, 64);
    close(fd);  /* 关闭文件 */
    if(r != 20) {
      printstr("read wrong length\n");
    } else {
      printstr("Test 2 passed: read back: ");
      printstr(buf);  /* 应输出 "Hello, File System!\n" */
    }
  }

  /* ---- Lab6 测试：fork/wait/exit ---- */
  int child = fork();  /* 创建子进程 */
  if (child == 0) {
    /* 子进程 */
    printstr("Child: hello\n");
    exit(1);           /* 子进程退出，状态码=1 */
  } else {
    /* 父进程 */
    int status;
    int cpid = wait(&status);  /* 等待子进程退出 */
    printstr("Parent: child ");
    printnum(cpid);
    printstr(" exited with ");
    printnum(status);
    printchar('\n');
  }

  exit(0);  /* 进程退出 */
}

/* 入口点 — 放在 .text.init 段，链接器脚本保证它排在最前面
 * _start 是内核在 userinit() 中设置的初始 PC */
void _start(void) __attribute__((section(".text.init")));
void _start(void) {
  main();    /* 调用主函数 */
  exit(0);   /* main 返回后安全退出 */
}

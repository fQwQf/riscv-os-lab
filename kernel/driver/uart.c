// 定义 UART 控制器的基地址(这是 QEMU virt 平台定死的物理地址)
#define UART0 0x10000000L

// 强转为指针。重要陷阱:必须通过 volatile 关键字告诉C 编译器,
// 千万别把这段看似“重复无意义”的赋值代码给优化(删)掉!
#define Reg(reg) ((volatile unsigned char *)(UART0 + reg))

void uart_putc(char c) {
    // 将字符 c 直接写入硬件寄存器!
    *Reg(0) = c;
}

// 实现打印字符串函数
void uart_puts(char *s) {
    // 遍历字符串的每个字符，循环调用 uart_putc，直到遇到 '\0' 结束符
    while (*s != '\0') {
        uart_putc(*s);
        s++;
    }
}
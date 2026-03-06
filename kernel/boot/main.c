// 声明在其他文件中实现的驱动函数
extern void uart_puts(char *s);

void start_main() {
    uart_puts("Hello OS from RISC-V Bare-metal!\n");
    
    // 操作系统作为一个服务程序，永远不应该自己退出，进入死循环待命
    while(1);
}   
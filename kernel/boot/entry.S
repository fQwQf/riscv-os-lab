    .section .text 
    .global _entry 
_entry: 
    # 1. 找到事先在内存中划好的连续栈空间的首地址 
    la sp, stack0

    # 2. 我们给栈分配 4KB(即 1页=4096 字节)的大小 
    li a0, 4096

    # 3.计算真正的栈顶。因为栈是由高地址向低地址生长的,所以 sp 必须设在最高处! 
    add sp, sp, a0

    # 4. 栈终于准备好了,现在可以安全跳转到了 C 代码的主函数(不再限定叫 main)
    call start_main

    # 5. 万一 C 函数不小心 return 退出了,我们需要一个死循环进行兜底,防止 CPU 甚至跑到乱码内存里去 
spin: 
    j spin

    # 下方是静态划分出的一块 4KB 内存数据区，留给栈专门使用
    .section .bss
    .align 16
stack0:
    .space 4096 
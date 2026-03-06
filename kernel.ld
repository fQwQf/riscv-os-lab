OUTPUT_ARCH( "riscv" )
ENTRY( _entry ) /* 告诉链接器：整个程序的真正入口点在 _entry 标签处 */

SECTIONS
{
    /* 确保内核代码从这个规定的固定物理地址开始 */
    . = 0x80000000;
    
    .text : {
        *(.text .text.*) /* 所有的代码段指令，都紧挨着这里存放 */
    }
    
    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.sbss .sbss.*) /* 小 BSS 段通常也会放这里 */
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }
}
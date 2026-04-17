/* user.ld — 用户程序链接脚本
 *
 * 将 .text.init 段放在最前面（_start 入口点），
 * 确保用户程序加载后从第一条指令开始正确执行。
 * 链接地址设为 0，配合 -mcmodel=medany 的 PC 相对寻址，
 * 代码可在任意物理地址运行。
 *
 * 关键：.rodata 合并到 .text 中，使字符串常量与代码在
 * 同一段内，确保 PC 相对寻址（auipc+addi）能正确访问。
 */

OUTPUT_ARCH(riscv)
ENTRY(_start)

SECTIONS {
    . = 0;

    .text : {
        *(.text.init)
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr)
        *(.comment)
        *(.note .note.*)
    }
}

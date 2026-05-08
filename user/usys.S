/* usys.S — 用户态系统调用汇编桩（Lab6 任务1）
 *
 * 每个"系统调用桩"都是一个 C 可调用的汇编函数。
 * 用户程序调用 getpid() 时，实际上是跳入这里的汇编代码：
 *   1. 将系统调用号加载到 a7 寄存器
 *   2. 执行 ecall 指令（触发陷阱，陷入内核）
 *   3. 内核处理完毕后，返回值已经放在 a0 寄存器中
 *   4. ret 指令返回到用户 C 代码，a0 就是函数返回值
 *
 * 这里的系统调用号必须与 kernel/syscall/syscall.c 中
 * syscalls[] 数组的下标保持一致！
 */

/* 系统调用号常量（与内核 syscall.c 中的定义一致）*/
#define SYS_fork    1
#define SYS_exit    2
#define SYS_wait    3
#define SYS_getpid  11
#define SYS_sbrk    12
#define SYS_write   16
#define SYS_open    15
#define SYS_read    13
#define SYS_close   17

/* ================================================================
 * TODO [Lab6-任务1]：
 *   理解下面的 getpid 汇编桩，然后仿照它实现 sys_write 的桩。
 *
 *   getpid() 已完整提供，你需要实现 write() 的桩。
 * ================================================================ */

/* getpid — 获取当前进程 PID（已提供完整实现）*/
    .global getpid
getpid:
    li a7, SYS_getpid   /* 将系统调用号 11 装入 a7 */
    ecall               /* 敲响警钟，陷入内核 */
    ret                 /* 内核返回后，a0 中就是 pid，ret 将其带回给调用者 */

/* exit — 终止当前进程（已提供完整实现）*/
    .global exit
exit:
    li a7, SYS_exit
    ecall
    ret

/* write — 向文件描述符写数据 */
    .global write
write:
    li a7, SYS_write
    ecall
    ret

/* fork — 创建子进程 */
    .global fork
fork:
    li a7, SYS_fork
    ecall
    ret

/* wait — 等待子进程退出 */
    .global wait
wait:
    li a7, SYS_wait
    ecall
    ret

/* open — 打开或创建文件 */
    .global open
open:
    li a7, SYS_open
    ecall
    ret

/* read — 从文件读数据 */
    .global read
read:
    li a7, SYS_read
    ecall
    ret

/* close — 关闭文件描述符 */
    .global close
close:
    li a7, SYS_close
    ecall
    ret

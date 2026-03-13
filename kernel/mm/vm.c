/* vm.c — 虚拟内存与页表管理（Lab3 任务2-4）
 *
 * 本文件实现 RISC-V Sv39 三级分页机制：
 *   - walk()      : 在三级页表中查找（并按需分配中间级页表）
 *   - mappages()  : 批量建立虚拟地址到物理地址的映射
 *   - kvmininit() : 建立内核页表（映射内核各段和设备）
 *   - kvminithart(): 将页表写入 satp 寄存器，开启 MMU 分页
 *
 * 重要概念：
 *   虚拟地址（VA）→ MMU查页表 → 物理地址（PA）
 *   Sv39中VA分解：[38:30]=VPN[2], [29:21]=VPN[1], [20:12]=VPN[0],
 * [11:0]=页内偏移
 */

#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "riscv.h"
#include "types.h"

/* 内核根页表（全局变量，Lab3 建立后整个内核都使用它）*/
pagetable_t kernel_pagetable;

/* 外部符号（由链接脚本/编译器生成，标记内核各段边界）*/
extern char etext[];       /* 内核代码段结束地址 */
extern char end_address[]; /* 内核数据段结束地址 */

/* ================================================================
 * walk — 在三级页表中查找虚拟地址 va 对应的最终 PTE 指针
 *
 * 参数：
 *   pagetable — 根页表物理地址
 *   va        — 要查找的虚拟地址
 *   alloc     — 若中间级页表不存在：1=自动分配新页表，0=直接返回0
 *
 * 返回值：最底层（level-0）PTE 的指针；找不到时返回 0。
 *
 * Sv39 三级页表遍历（从 level-2 到 level-0）：
 *   每级用 PX(level, va) 提取 9 位索引，乘以8字节，找到对应 PTE。
 *   PTE 中取出物理页号（PPN），转为物理地址（下一级页表基地址）。
 * ================================================================ */
pte_t *walk(pagetable_t pagetable, uint64 va, int alloc) {
  if (va >= MAXVA)
    panic("walk: virtual address out of range");

  /* 从 level-2 到 level-1，共遍历两层（最后 level-0 由调用者处理）*/
  for (int level = 2; level > 0; level--) {
    /* 取当前层的 PTE 指针 */
    pte_t *pte = &pagetable[PX(level, va)];

    if (*pte & PTE_V) {
      /* 该 PTE 有效：提取下一级页表的物理地址 */
      /* ================================================================
       * TODO [Lab3-任务2-步骤1]：
       *   从 *pte 中提取物理页号，转为下一级页表的物理基地址。
       *   提示：pagetable = (pagetable_t)PTE2PA(*pte);
       * ================================================================ */
    } else {
      /* 该 PTE 无效：中间级页表不存在 */
      if (!alloc)
        return 0; /* 不允许分配，返回失败 */

      /* 分配一个新的物理页作为下一级页表 */
      pagetable = (pagetable_t)kalloc();
      if (pagetable == 0)
        return 0; /* 内存耗尽 */

      /* 新页表必须清零！否则随机数据会被当成有效 PTE */
      /* ================================================================
       * TODO [Lab3-任务2-步骤2]：
       *   调用 memset 或手动循环，将新分配的 pagetable 清零（PGSIZE字节）。
       *   提示：for (int i = 0; i < PGSIZE/8; i++) ((uint64*)pagetable)[i] = 0;
       * ================================================================ */

      /* ================================================================
       * TODO [Lab3-任务2-步骤3]：
       *   将新页表的物理地址写入当前 PTE，并设置 PTE_V（有效位）。
       *   提示：*pte = PA2PTE(pagetable) | PTE_V;
       * ================================================================ */
    }
  }

  /* 返回 level-0 页表中对应的 PTE 指针（最终映射层）*/
  return &pagetable[PX(0, va)];
}

/* ================================================================
 * mappages — 建立从虚拟地址范围到物理地址范围的映射
 *
 * 参数：
 *   pagetable — 根页表
 *   va        — 虚拟地址起始（会被向下对齐到页边界）
 *   size      — 映射大小（字节）
 *   pa        — 对应的物理地址起始
 *   perm      — 权限位（PTE_R/PTE_W/PTE_X/PTE_U 的组合）
 *
 * 返回值：0 表示成功，-1 表示失败（内存不足）
 * ================================================================ */
int mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa,
             int perm) {
  uint64 a, last;
  pte_t *pte;

  if (size == 0)
    panic("mappages: size is 0");

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);

  for (;;) {
    /* 找到 va=a 对应的 level-0 PTE（必要时分配中间页表）*/
    if ((pte = walk(pagetable, a, 1)) == 0)
      return -1;

    /* 重复映射是内核 Bug */
    if (*pte & PTE_V)
      panic("mappages: remap");

    /* ================================================================
     * TODO [Lab3-任务3]：
     *   将物理地址 pa 和权限 perm 以及有效位 PTE_V 写入 *pte。
     *   提示：*pte = PA2PTE(pa) | perm | PTE_V;
     * ================================================================ */

    if (a == last)
      break;

    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

/* ================================================================
 * kvmininit — 建立内核页表
 *
 * 建立的映射关系（"恒等映射"：虚拟地址 = 物理地址，方便内核访问）：
 *   UART0   设备 → 可读写
 *   内核代码段   → 可读+可执行
 *   内核数据段   → 可读+可写
 *   可用物理内存 → 可读+可写
 *
 * 注：更完整的版本还需映射 PLIC、virtio 等设备（Lab7使用）。
 * ================================================================ */
void kvmininit(void) {
  /* 分配根页表 */
  kernel_pagetable = (pagetable_t)kalloc();
  if (kernel_pagetable == 0)
    panic("kvmininit: out of memory");

  /* 清零根页表 */
  for (int i = 0; i < PGSIZE / 8; i++)
    ((uint64 *)kernel_pagetable)[i] = 0;

  /* ================================================================
   * TODO [Lab3-任务4-步骤1]：
   *   映射 UART0 串口设备（MMIO区域）：
   *     mappages(kernel_pagetable, UART0, PGSIZE, UART0, PTE_R | PTE_W);
   *
   *   （提示：UART0 在 memlayout.h 中定义为 0x10000000）
   * ================================================================ */

  /* ================================================================
   * TODO [Lab3-任务4-步骤2]：
   *   映射内核代码段：从 KERNBASE 到 etext，权限为可读+可执行。
   *     mappages(kernel_pagetable, KERNBASE, (uint64)etext-KERNBASE,
   *              KERNBASE, PTE_R | PTE_X);
   * ================================================================ */

  /* ================================================================
   * TODO [Lab3-任务4-步骤3]：
   *   映射内核数据段和剩余可用物理内存：从 etext 到 PHYSTOP，权限为可读+可写。
   *     mappages(kernel_pagetable, (uint64)etext, PHYSTOP-(uint64)etext,
   *              (uint64)etext, PTE_R | PTE_W);
   * ================================================================ */
}

/* ================================================================
 * kvminithart — 开启当前 CPU 核心的 MMU 分页
 *
 * 将 kernel_pagetable 写入 satp 寄存器，告诉 MMU：
 *   "从现在起，所有地址访问都要经过你查页表翻译！"
 *
 * 注意：执行完这条指令后，CPU 立即开始查页表。
 *       如果 kvmininit 的映射写错了，下一条指令就会产生 Page Fault，
 *       导致系统崩溃（此时无任何错误提示，GDB 单步调试是唯一出路）。
 * ================================================================ */
void kvminithart(void) {
  /* ================================================================
   * TODO [Lab3-任务4-步骤4]：
   *   1. 将根页表地址写入 satp 寄存器（开启Sv39分页）：
   *        w_satp(MAKE_SATP(kernel_pagetable));
   *   2. 刷新 TLB（清空CPU中缓存的旧地址翻译结果）：
   *        sfence_vma();
   *
   *   这两步的顺序不能颠倒！
   * ================================================================ */
}

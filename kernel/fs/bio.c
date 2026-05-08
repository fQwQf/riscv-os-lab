/* bio.c — 块设备缓冲层（Lab7 任务1）
 *
 * 【这一层解决了什么问题？】
 * 磁盘访问极慢（相比 CPU），直接每次都调 virtio_disk_rw() 代价很高。
 * 缓冲层在内存中维护一个缓冲池（Buffer Cache）：
 *   - 第一次读块 X → 从磁盘读，存入内存缓冲
 *   - 第二次读块 X → 直接从内存返回（缓存命中）
 *   - 修改块 X 的内容 → 在内存中修改，通过 bwrite 写回磁盘
 *
 * 【更关键的作用：唯一性保证】
 * 缓冲层保证同一时刻每个磁盘块在内存中只有一份拷贝。
 * 否则两个进程分别修改同一块的两份拷贝，写回时产生数据冲突。
 *
 * 【数据结构】
 *   struct buf  — 内存中缓存一个磁盘块（1024字节）的结构体
 *   bcache      — 所有 buf 的缓冲池，形成双向 LRU 链表
 *
 * 【核心操作】
 *   bread(dev, blockno) — 读取磁盘块（优先从缓存取，没有则从磁盘读）
 *   bwrite(b)           — 将修改过的缓冲块写回磁盘
 *   brelse(b)           — 释放缓冲块的占用（归还给缓冲池）
 *
 * 【LRU 策略】
 *   brelse 把刚释放的块移到链表头部（最近使用，暂不淘汰）
 *   bget 从链表尾部往前找 refcnt==0 的块（最久没人用的）淘汰
 */

#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "riscv.h"
#include "types.h"

#include "types.h"
#include "buf.h"

/* 缓冲池（全局，内核中只有一个）
 *
 * buf[NBUF]: 固定数量的缓冲区数组（NBUF=30，在 param.h 中定义）
 * head:      LRU 双向循环链表的哨兵头节点（不存数据，只做链表管理）
 *
 * 链表排列（LRU 语义）：
 *   head ↔ [最近刚用完 A] ↔ [较新 B] ↔ [较旧 C] ↔ [最久没用 D] ↔ head
 *          ↑ brelse 头插                  ↑ bget 从尾部扫描淘汰
 */
struct {
  struct buf buf[NBUF];
  struct buf head;
} bcache;

/* 外部磁盘驱动函数（在 virtio_disk.c 中实现，本框架中已提供）
 * 这是整个文件系统中唯一真正接触硬件的函数 */
extern void virtio_disk_rw(struct buf *b, int write);

/* ================================================================
 * binit — 初始化缓冲池（内核启动时由 fsinit → binit 调用）
 *
 * 将所有 buf 通过头插法连成双向循环链表：
 *   head ↔ buf[0] ↔ buf[1] ↔ ... ↔ buf[NBUF-1] ↔ head
 *
 * 初始状态所有 buf 的 refcnt=0（没人用）、valid=0（无有效数据）
 * ================================================================ */
void binit(void) {
  struct buf *b;

  /* 初始化哨兵：prev 和 next 都指向自己（空链表）*/
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;

  /* 头插法：每次把新节点插到 head 后面
   *
   * 插入前：head ↔ 已有节点链
   * 插入后：head ↔ 新节点 ↔ 已有节点链
   *
   * 需要修改 4 个指针：
   *   b->next = head->next          新节点指向原第一个节点
   *   b->prev = &bcache.head        新节点的前驱是 head
   *   head->next->prev = b          原第一个节点的前驱改为新节点
   *   head->next = b                head 的后继改为新节点
   */
  for (b = bcache.buf; b < bcache.buf + NBUF; b++) {
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
}

/* ================================================================
 * bget — 在缓冲池中找到指定 (dev, blockno) 的缓冲块
 *
 * 算法：
 *   步骤1: 正向遍历链表，若找到 dev 和 blockno 匹配的 buf，
 *          引用计数+1，直接返回（缓存命中）
 *   步骤2: 若未命中，反向遍历链表（从尾部开始 = LRU 最久未用），
 *          找第一个 refcnt==0 的空闲块，重用为新的缓存
 *   步骤3: 若全部 buf 都在被使用，panic（缓冲池耗尽）
 *
 * ⚠ 注意：bget 返回的 buf 可能 valid=0（刚被重用），数据尚未从磁盘读取。
 *         调用者（bread）会检查 valid 并在需要时触发真正的磁盘 I/O。
 *
 * ⚠ 这就是"同一磁盘块在内存中只有一份拷贝"的保证：
 *         步骤1 先检查是否已有缓存，有就直接复用，不会分配第二份。
 * ================================================================ */
static struct buf *bget(uint dev, uint blockno) {
  struct buf *b;

  /* 步骤1：查找是否已缓存（正向遍历，从链表头部开始）
   * 如果找到 dev 和 blockno 匹配且正在使用的 buf，递增引用计数 */
  for (b = bcache.head.next; b != &bcache.head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;  /* 有人要用，引用计数+1 */
      return b;
    }
  }

  /* 步骤2：LRU 淘汰——从链表尾部（最久未使用）向前找空闲块
   * refcnt==0 表示当前没有人使用这个缓冲块，可以回收 */
  for (b = bcache.head.prev; b != &bcache.head; b = b->prev) {
    if (b->refcnt == 0) {
      /* 重用这个空闲缓冲块 */
      b->dev = dev;           /* 更新为目标设备号 */
      b->blockno = blockno;   /* 更新为目标块号 */
      b->valid = 0;           /* 旧数据已失效，调用者需要重新从磁盘读 */
      b->refcnt = 1;          /* 标记为正在使用 */
      return b;
    }
  }

  /* 步骤3：所有缓冲块都被占用（refcnt > 0），无法满足请求 */
  panic("bget: no buffers");
}

/* ================================================================
 * bread — 读取磁盘块（返回含有效数据的缓冲块指针）
 *
 * 流程：
 *   1. bget(dev, blockno) — 从缓冲池获取该块的缓存项
 *   2. 若 valid==0（缓存未命中或刚被重用）：调 virtio_disk_rw 真正读磁盘
 *   3. 标记 valid=1（数据已有效）
 *   4. 返回 buf 指针（调用者用完后必须 brelse）
 *
 * ⚠ 调用者拿到 buf 后可以自由读取 buf->data[]。
 *    修改后需要调用 bwrite 写回磁盘。
 *    使用完毕后必须调用 brelse 释放。
 * ================================================================ */
struct buf *bread(uint dev, uint blockno) {
  struct buf *b;

  b = bget(dev, blockno);  /* 获取缓冲块（可能命中缓存，可能分配新块） */

  if (!b->valid) {
    /* 缓存未命中：数据是旧的或无效的，需要从磁盘真正读取
     * virtio_disk_rw(b, 0) 中第二个参数 0 表示读操作
     * 读完后 b->data[] 中就是该磁盘块的 1024 字节内容 */
    virtio_disk_rw(b, 0);
    b->valid = 1;  /* 标记数据有效，下次再 bread 同一块就直接返回 */
  }

  return b;
}

/* ================================================================
 * bwrite — 将缓冲块的修改写回磁盘
 *
 * ⚠ 调用场景：修改了 buf->data[] 后，必须调用 bwrite 持久化到磁盘。
 *    如果省略 bwrite，修改只在内存中，系统崩溃后数据丢失。
 *
 * 注意：bwrite 只做写操作，不改变 valid（写后数据仍然有效）。
 * ================================================================ */
void bwrite(struct buf *b) {
  /* virtio_disk_rw(b, 1) 中第二个参数 1 表示写操作
   * 将 b->data[] 的 1024 字节写入磁盘的 b->blockno 号块 */
  virtio_disk_rw(b, 1);
}

/* ================================================================
 * brelse — 使用完毕，释放对缓冲块的占用
 *
 * 流程：
 *   1. refcnt-- （引用计数减 1）
 *   2. 若 refcnt 归零（没人用了）：
 *      a. 从当前位置摘除
 *      b. 重新头插到 bcache.head 后面（LRU 核心！）
 *
 * 【为什么头插？】
 *   刚释放的块是"最近使用过的"，应该排在链表头部附近。
 *   下次 bget 需要淘汰时从尾部找，最久没用的是最先被淘汰的。
 *   这就是 LRU（Least Recently Used）的实现原理。
 * ================================================================ */
void brelse(struct buf *b) {
  b->refcnt--;  /* 引用计数减 1 */

  if (b->refcnt == 0) {
    /* 没人再用这个块了，移到链表头部（标记为"最近使用"） */

    /* 步骤 a：从当前位置摘除
     * b->next->prev = b->prev   后继的前驱跳过我
     * b->prev->next = b->next   前驱的后继跳过我 */
    b->next->prev = b->prev;
    b->prev->next = b->next;

    /* 步骤 b：头插到 head 后面
     * head ↔ [我] ↔ [原来的第一个节点] ↔ ... */
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  /* 如果 refcnt > 0，说明还有人在用这个块，什么都不做 */
}

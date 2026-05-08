/* virtio_disk.c — VirtIO 块设备驱动（xv6 风格）
 *
 * 通过 VirtIO MMIO 接口与 QEMU 模拟的磁盘通信。
 * 实现一个极简的同步 I/O：每次请求都等磁盘完成后才返回。
 */

#include "types.h"
#include "buf.h"
#include "defs.h"
#include "memlayout.h"
#include "param.h"
#include "riscv.h"

/* ================================================================
 * VirtIO 规范常量（VirtIO 1.0 / MMIO）
 * ================================================================ */
#define VIRTIO_MMIO_MAGIC_VALUE     0x000  /* 必须为 0x74726976 */
#define VIRTIO_MMIO_VERSION         0x004  /* 必须为 1 或 2 */
#define VIRTIO_MMIO_DEVICE_ID       0x008  /* 2 = 块设备 */
#define VIRTIO_MMIO_VENDOR_ID       0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_GUEST_PAGE_SIZE 0x028  /* version 1 only */
#define VIRTIO_MMIO_QUEUE_SEL       0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX   0x034
#define VIRTIO_MMIO_QUEUE_NUM       0x038
#define VIRTIO_MMIO_QUEUE_ALIGN     0x03c  /* version 1 only */
#define VIRTIO_MMIO_QUEUE_PFN       0x040  /* version 1 only */
#define VIRTIO_MMIO_QUEUE_READY     0x044  /* version 2 only */
#define VIRTIO_MMIO_QUEUE_NOTIFY    0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064
#define VIRTIO_MMIO_STATUS          0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW  0x080  /* version 2 */
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW 0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW  0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH 0x0a4
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0fc
#define VIRTIO_MMIO_CONFIG          0x100

/* VirtIO 设备状态位 */
#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#define VIRTIO_CONFIG_S_DRIVER      2
#define VIRTIO_CONFIG_S_DRIVER_OK   4
#define VIRTIO_CONFIG_S_FEATURES_OK 8

/* VirtIO 块设备特性位 */
#define VIRTIO_BLK_F_RO             5
#define VIRTIO_BLK_F_SCSI           7
#define VIRTIO_BLK_F_CONFIG_WCE     11
#define VIRTIO_BLK_F_MQ             12
#define VIRTIO_F_ANY_LAYOUT         27
#define VIRTIO_RING_F_INDIRECT_DESC 28
#define VIRTIO_RING_F_EVENT_IDX     29

/* Virtqueue 描述符标志 */
#define VRING_DESC_F_NEXT  1  /* 还有下一个描述符 */
#define VRING_DESC_F_WRITE 2  /* 只写（设备写，驱动读）*/

/* VirtIO 块请求类型 */
#define VIRTIO_BLK_T_IN  0  /* 读磁盘 */
#define VIRTIO_BLK_T_OUT 1  /* 写磁盘 */

/* Virtqueue 大小（必须是2的幂）*/
#define NUM 8

/* ================================================================
 * Virtqueue 数据结构（与 VirtIO 规范一致）
 * ================================================================ */

/* 描述符表项 */
struct virtq_desc {
  uint64 addr;   /* 数据的物理地址 */
  uint32 len;    /* 数据长度 */
  uint16 flags;  /* VRING_DESC_F_* */
  uint16 next;   /* 下一个描述符的索引 */
};

/* Available Ring（驱动填写，告诉设备哪些描述符链可用）*/
struct virtq_avail {
  uint16 flags;     /* 通常为 0 */
  uint16 idx;       /* 驱动写入的下一个位置 */
  uint16 ring[NUM]; /* 描述符链头部索引 */
};

/* Used Ring 中每个条目 */
struct virtq_used_elem {
  uint32 id;  /* 描述符链头部索引 */
  uint32 len; /* 设备写入的字节数 */
};

/* Used Ring（设备填写，告诉驱动哪些请求完成了）*/
struct virtq_used {
  uint16 flags;
  uint16 idx;                      /* 设备写入的下一个位置 */
  struct virtq_used_elem ring[NUM];
};

/* ================================================================
 * 块请求头（放在描述符链的第一个描述符）
 * ================================================================ */
struct virtio_blk_req {
  uint32 type;     /* VIRTIO_BLK_T_IN 或 VIRTIO_BLK_T_OUT */
  uint32 reserved;
  uint64 sector;   /* 扇区号（每扇区512字节）*/
};

/* ================================================================
 * 驱动状态（全局，单个 virtqueue）
 * ================================================================ */
static struct {
  /* 实际的 virtqueue 内存（需要页对齐）*/
  __attribute__((aligned(4096))) struct virtq_desc desc[NUM];
  __attribute__((aligned(2))) struct virtq_avail avail;
  __attribute__((aligned(4096))) struct virtq_used used;

  /* 追踪每个描述符链是否空闲 */
  char free[NUM];

  /* used->idx 的上一个值，用于检测新完成的请求 */
  uint16 used_idx;

  /* 每个槽位的未完成请求信息（用于同步等待）*/
  struct {
    struct buf *b;     /* 对应的 buf */
    char status;       /* 设备填写的完成状态（0=成功）*/
  } info[NUM];

  /* 请求头（每个槽位一个，持久存放）*/
  struct virtio_blk_req ops[NUM];
} disk;

/* 读写 VirtIO MMIO 寄存器的辅助宏 */
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

/* ================================================================
 * virtio_disk_init — 初始化 VirtIO 块设备
 * ================================================================ */
void virtio_disk_init(void) {
  uint32 status = 0;

  if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
     *R(VIRTIO_MMIO_VERSION) != 1 ||
     *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
     *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
    panic("virtio disk init failed: bad magic/version/device");
  }

  /* 1. Reset */
  *R(VIRTIO_MMIO_STATUS) = status;

  /* 2. ACKNOWLEDGE */
  status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
  *R(VIRTIO_MMIO_STATUS) = status;

  /* 3. DRIVER */
  status |= VIRTIO_CONFIG_S_DRIVER;
  *R(VIRTIO_MMIO_STATUS) = status;

  /* 4. 协商特性（不需要任何高级特性）*/
  uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
  features &= ~(1 << VIRTIO_BLK_F_RO);
  features &= ~(1 << VIRTIO_BLK_F_SCSI);
  features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
  features &= ~(1 << VIRTIO_BLK_F_MQ);
  features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
  features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
  features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
  *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

  /* 5. FEATURES_OK */
  status |= VIRTIO_CONFIG_S_FEATURES_OK;
  *R(VIRTIO_MMIO_STATUS) = status;

  /* 6. 设置 guest page size（version 1 需要）*/
  *R(VIRTIO_MMIO_GUEST_PAGE_SIZE) = 4096;

  /* 7. 初始化 Queue 0 */
  *R(VIRTIO_MMIO_QUEUE_SEL) = 0;
  uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
  if(max == 0) panic("virtio disk: no queue");
  if(max < NUM) panic("virtio disk: queue too short");

  *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;
  *R(VIRTIO_MMIO_QUEUE_ALIGN) = 4096;
  /* 将描述符表的物理地址（以页为单位）写入 */
  *R(VIRTIO_MMIO_QUEUE_PFN) = ((uint64)disk.desc) >> 12;

  /* 8. 标记 free 表（所有描述符空闲）*/
  for(int i = 0; i < NUM; i++)
    disk.free[i] = 1;

  /* 9. DRIVER_OK */
  status |= VIRTIO_CONFIG_S_DRIVER_OK;
  *R(VIRTIO_MMIO_STATUS) = status;
}

/* 分配一个空闲描述符，返回其索引；无空闲时 panic */
static int alloc_desc(void) {
  for(int i = 0; i < NUM; i++){
    if(disk.free[i]){
      disk.free[i] = 0;
      return i;
    }
  }
  panic("virtio: no desc");
  return -1;
}

/* 释放描述符链 */
static void free_chain(int i) {
  while(1){
    int next = disk.desc[i].next;
    int flag = disk.desc[i].flags;
    disk.free[i] = 1;
    disk.desc[i].flags = 0;
    if(flag & VRING_DESC_F_NEXT)
      i = next;
    else
      break;
  }
}

/* ================================================================
 * virtio_disk_rw — 同步读写一个磁盘块
 *
 * b->blockno  : 块号（每块 BSIZE=1024 字节 = 2 个 512B 扇区）
 * write == 0  : 读（磁盘 → b->data）
 * write == 1  : 写（b->data → 磁盘）
 * ================================================================ */
void virtio_disk_rw(struct buf *b, int write) {
  uint64 sector = (uint64)b->blockno * (BSIZE / 512);

  /* 分配 3 个描述符（请求头 + 数据 + 状态）*/
  int idx[3];
  for(int i = 0; i < 3; i++)
    idx[i] = alloc_desc();

  /* 描述符 0：请求头 */
  struct virtio_blk_req *buf0 = &disk.ops[idx[0]];
  buf0->type = write ? VIRTIO_BLK_T_OUT : VIRTIO_BLK_T_IN;
  buf0->reserved = 0;
  buf0->sector = sector;

  disk.desc[idx[0]].addr  = (uint64)buf0;
  disk.desc[idx[0]].len   = sizeof(struct virtio_blk_req);
  disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
  disk.desc[idx[0]].next  = idx[1];

  /* 描述符 1：数据缓冲区 */
  disk.desc[idx[1]].addr  = (uint64)b->data;
  disk.desc[idx[1]].len   = BSIZE;
  /* 读操作：设备写入缓冲区（WRITE标志表示对驱动只写）*/
  disk.desc[idx[1]].flags = (write ? 0 : VRING_DESC_F_WRITE) | VRING_DESC_F_NEXT;
  disk.desc[idx[1]].next  = idx[2];

  /* 描述符 2：状态字节（设备填写）*/
  disk.info[idx[0]].status = 0xff; /* 初始为非0，设备成功后置0 */
  disk.desc[idx[2]].addr  = (uint64)&disk.info[idx[0]].status;
  disk.desc[idx[2]].len   = 1;
  disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; /* 设备写 */
  disk.desc[idx[2]].next  = 0;

  /* 记录 buf，供中断/轮询识别 */
  disk.info[idx[0]].b = b;
  b->disk = 1;

  /* 提交到 Available Ring */
  disk.avail.ring[disk.avail.idx % NUM] = idx[0];
  __sync_synchronize();
  disk.avail.idx++;
  __sync_synchronize();

  /* 通知设备（kick）*/
  *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0;

  /* ---- 忙等待，直到设备完成（同步驱动，无需中断）---- */
  while(b->disk == 1){
    /* 检查 used ring 是否有新完成项 */
    __sync_synchronize();
    while(disk.used_idx != disk.used.idx){
      __sync_synchronize();
      int id = disk.used.ring[disk.used_idx % NUM].id;
      if(disk.info[id].status != 0)
        panic("virtio_disk_rw: status error");
      disk.info[id].b->disk = 0;
      disk.used_idx++;
    }
  }

  /* 释放描述符链 */
  free_chain(idx[0]);
}

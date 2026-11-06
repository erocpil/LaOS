#ifndef __NIC_E1000_H__
#define __NIC_E1000_H__

#include <stdint.h>
#include "../kernel/lock.h"

#define NIC_VIRT_BASE 0xFFFFD00000000000

#define E1000_VENDER 0x8086
#define E1000_DEVICE 0x100E

/* 寄存器定义 */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_EERD   0x0014
#define REG_ICR	   0x00C0
#define REG_IMS	   0x00D0
#define REG_IMC	   0x00D8

#define E1000_INT_RXDMT0 (1U << 4)
#define E1000_INT_RXO	 (1U << 6)
#define E1000_INT_RXT0	 (1U << 7)
#define REG_RCTL	 0x0100
#define REG_RDBAL	 0x2800
#define REG_RDBAH	 0x2804
#define REG_RDLEN	 0x2808
#define REG_RDH		 0x2810
#define REG_RDT		 0x2818
#define REG_SWSM	 0x5B50
#define REG_RAL		 0x5400
#define REG_RAH		 0x5404

/* 发送控制与描述符 (Transmit) */
#define REG_TCTL  0x0400 /* 发送控制寄存器。 */
#define TCTL_EN   (1 << 1)
#define REG_TDBAL 0x3800 /* 发送描述符环基地址低 32 位。 */
#define REG_TDBAH 0x3804 /* 发送描述符环基地址高 32 位。 */
#define REG_TDLEN 0x3808 /* 发送描述符环长度。 */
#define REG_TDH	  0x3810 /* 发送描述符环头索引。 */
#define REG_TDT	  0x3818 /* 发送描述符环尾索引。 */

/* 硬件统计计数器，定义参见 Intel 82540EM 手册第 13.4 节。
 *
 * 所有寄存器都是"读即清 (RC)"语义：读一次硬件清零，下次读到的是本次读到
 * 两次读取之间新累积的计数。软件须将每次读取值累加到 64 位软件计数器，
 * 才能得到自启动以来的累计值。多个上下文竞争读取同一寄存器会导致计数丢失，
 * 因此硬件计数刷新只由 rx_process_thread 一处按 500ms 频率触发。
 *
 * GORCL/H,GOTCL/H 是 64bit 计数拆两个 32bit 寄存器：读 L 时硬件把 H
 * 锁存高位；读取高位后，硬件才清除这组计数。必须先读低位、再读高位，
 * 以取得同一计数值的两个部分。 */
#define REG_CRCERRS 0x4000 /* CRC 错误计数。 */
#define REG_MPC	    0x4010 /* 接收缓冲区溢出导致的丢包计数。 */
#define REG_COLC    0x4028 /* 冲突计数。 */
#define REG_GPRC    0x4074 /* 正确接收的数据包计数。 */
#define REG_GPTC    0x4080 /* 正确发送的数据包计数。 */
#define REG_GORCL   0x4088 /* 接收字节计数低 32 位。 */
#define REG_GORCH   0x408C /* 接收字节计数高 32 位。 */
#define REG_GOTCL   0x4090 /* 发送字节计数低 32 位。 */
#define REG_GOTCH   0x4094 /* 发送字节计数高 32 位。 */
#define REG_RNBC    0x40A0 /* 接收无可用缓冲区计数。 */
#define REG_TPR	    0x40D0 /* 接收数据包总数，包含错误包。 */
#define REG_TPT	    0x40D4 /* 发送数据包总数。 */

/* RCTL 位定义 */
#define RCTL_EN	     (1 << 1)
#define RCTL_SBP     (1 << 2)
#define RCTL_UPE     (1 << 3)
#define RCTL_MPE     (1 << 4)
#define RCTL_LPE     (1 << 5)
#define RCTL_BAM     (1 << 15)
#define RCTL_SZ_2048 (0 << 16)
#define RCTL_SECRC   (1 << 26)

#define E1000_TXD_STAT_DD 0x01 /* 描述符处理完成。 */

#define SWSM_SWESMBI (1 << 1)

#define TX_CMD_EOP  (1 << 0) /* 数据包结束。 */
#define TX_CMD_IFCS (1 << 1) /* 由硬件插入帧校验序列。 */
#define TX_CMD_RS   (1 << 3) /* 请求硬件回写完成状态。 */

/* DMA 描述符由设备与 CPU 共享。volatile 约束编译器对字段访问的处理，
 * 但不负责缓存维护，也不单独提供 CPU 与设备之间的内存顺序。
 * 驱动依赖设备先写入描述符数据、最后置位 DD 的协议；在读取 DD 和数据
 * 前后，须按目标架构执行所需的 DMA 同步操作。AArch64 的非一致性 DMA
 * 路径通过 arch_cache.h 执行缓存维护。
 */
struct e1000_rx_desc {
	volatile uint64_t buffer_addr; /* CPU 写入缓冲区物理地址，设备读取。 */
	volatile uint16_t length;
	volatile uint16_t checksum;
	volatile uint8_t status;
	volatile uint8_t errors;
	volatile uint16_t special;
} __attribute__((packed));

#define NUM_RX_DESC	     256
#define NUM_TX_DESC	     256
#define E1000_TX_BUFFER_SIZE 4096

/* 发送描述符布局；设备写回的 status 字段需保持可观察的内存访问。 */
struct e1000_tx_desc {
	uint64_t buffer_addr;
	uint16_t length;
	uint8_t cso;		 /* 校验和字段偏移。 */
	uint8_t cmd;		 /* 命令位，如 EOP、IFCS 和 RS。 */
	volatile uint8_t status; /* 设备写回的完成状态。 */
	uint8_t css;		 /* 校验和起始偏移。 */
	uint16_t special;
} __attribute__((packed));

typedef struct {
	/* MMIO 基本信息。 */
	uint64_t mmio_base;
	uint8_t mac[6];

	/* 接收环及其缓冲区。 */
	struct e1000_rx_desc *rx_ring; /* 虚拟地址。 */
	uint64_t rx_ring_phys;	       /* 物理地址。 */
	void *rx_buffers[NUM_RX_DESC];
	uint32_t rx_tail; /* 软件维护的接收环尾索引。 */

	/* 发送环及其缓冲区。 */
	struct e1000_tx_desc *tx_ring; /* 虚拟地址。 */
	uint64_t tx_ring_phys;	       /* 物理地址。 */
	void *tx_buffers[NUM_TX_DESC]; /* 每个发送描述符对应的缓冲区。 */
	uint32_t tx_tail;	       /* 下一个可提交的发送描述符索引。 */
	spinlock_t tx_lock;	       /* 串行化发送环及缓冲区的访问。 */
	int (*tx)(void *nic, void *data, uint16_t len);
} e1000_driver_t;

/* 批量接收的单个条目 */
typedef struct {
	void *data; /* 指向 rx_buffers 中的虚拟地址，不复制报文数据。 */
	uint16_t len;
	uint32_t desc_idx; /* 对应的接收描述符索引，归还时使用。 */
} e1000_rx_packet_t;

#define RX_BATCH_MAX 32

/* 批量轮询最多 max_batch 个就绪报文；不在此处更新 RDT。 */
int e1000_poll_packet_batch(e1000_driver_t *nic, e1000_rx_packet_t *out, int max_batch);

/* 调用方处理完报文后调用；统一更新 RDT，将描述符交还设备。 */
void e1000_rx_batch_release(e1000_driver_t *nic, e1000_rx_packet_t *batch, int count);


/* 接收线程替换描述符缓冲区后即可继续收包；处理线程独立消费旧缓冲区中的数据。 */
/* 一条待处理报文记录 */
typedef struct {
	void *data; /* 报文数据的虚拟地址，指向原接收缓冲区。 */
	uint16_t len;
	uint64_t buf_phys; /* 缓冲区物理地址，处理完成后用于回收。 */
} e1000_rx_entry_t;

#define RX_QUEUE_SIZE	  256 /* 待处理队列容量 */
#define RX_FREE_POOL_SIZE 256 /* 空闲页池容量；与接收描述符数量相同，以减少慢速分配。 */

void e1000_rx_pipeline_init(void);
int e1000_poll_and_enqueue(e1000_driver_t *nic);
int e1000_rx_dequeue_packet(e1000_rx_entry_t *out);
void e1000_rx_release_buffer(e1000_rx_entry_t *entry);


void e1000_map_bar(uint64_t bar0_phys);
void e1000_reset();
void e1000_acquire_eeprom();
void e1000_read_mac(uint8_t *mac);
void e1000_check_status();
e1000_driver_t *e1000_init(void *data);
void e1000_irq_handler(void);
void e1000_wait_rx_event(void);
int e1000_init_rx(e1000_driver_t *nic);
int e1000_init_tx(e1000_driver_t *nic);
void *e1000_poll_packet(e1000_driver_t *nic, uint16_t *out_len);
void e1000_test_loop(e1000_driver_t *nic);

#endif

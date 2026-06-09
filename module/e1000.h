#ifndef __NIC_E1000_H__
#define __NIC_E1000_H__

#include <stdint.h>
#include "../kernel/lock.h"

#define NIC_VIRT_BASE	0xFFFFD00000000000

#define E1000_VENDER	0x8086
#define E1000_DEVICE	0x100E

/* 寄存器定义 */
#define REG_CTRL     0x0000
#define REG_STATUS   0x0008
#define REG_EERD     0x0014
#define REG_ICR      0x00C0
#define REG_IMS      0x00D0
#define REG_IMC      0x00D8

#define E1000_INT_RXDMT0  (1U << 4)
#define E1000_INT_RXO     (1U << 6)
#define E1000_INT_RXT0    (1U << 7)
#define REG_RCTL     0x0100
#define REG_RDBAL    0x2800
#define REG_RDBAH    0x2804
#define REG_RDLEN    0x2808
#define REG_RDH      0x2810
#define REG_RDT      0x2818
#define REG_SWSM     0x5B50
#define REG_RAL      0x5400
#define REG_RAH      0x5404

/* 发送控制与描述符 (Transmit) */
#define REG_TCTL     0x0400  /* Transmit Control */
#define REG_TDBAL    0x3800  /* TX Descriptor Base Address Low */
#define REG_TDBAH    0x3804  /* TX Descriptor Base Address High */
#define REG_TDLEN    0x3808  /* TX Descriptor Length */
#define REG_TDH      0x3810  /* TX Descriptor Head */
#define REG_TDT      0x3818  /* TX Descriptor Tail */

/* 硬件统计计数器 (Statistical Registers) : Intel 82540EM 手册 S13.4
 *
 * 所有寄存器都是"读即清 (RC)"语义：读一次硬件清零，下次读到的是本次读到
 * 下次读之间新累积的量。软件必须每次读完把 delta 累加到自己的 64bit 总
 * 计数里，才能得到"上电以来累计"值。多个上下文竞争读同一寄存器会丢计数，
 * 因此硬件计数刷新只由 rx_process_thread 一处按 500ms 频率触发。
 *
 * GORCL/H,GOTCL/H 是 64bit 计数拆两个 32bit 寄存器：读 L 时硬件把 H
 * latch 住，读 H 时才把 L/H 一起清零。必须"先读 L 后读 H"，否则 L 已清
 * 拿到的是新累积。 */
#define REG_CRCERRS  0x4000  /* CRC Error Count */
#define REG_MPC      0x4010  /* Missed Packet Count (RX FIFO 满丢包) */
#define REG_COLC     0x4028  /* Collision Count */
#define REG_GPRC     0x4074  /* Good Packets Received */
#define REG_GPTC     0x4080  /* Good Packets Transmitted */
#define REG_GORCL    0x4088  /* Good Octets Received Low */
#define REG_GORCH    0x408C  /* Good Octets Received High */
#define REG_GOTCL    0x4090  /* Good Octets Transmitted Low */
#define REG_GOTCH    0x4094  /* Good Octets Transmitted High */
#define REG_RNBC     0x40A0  /* Receive No Buffers Count */
#define REG_TPR      0x40D0  /* Total Packets Received (含错) */
#define REG_TPT      0x40D4  /* Total Packets Transmitted */

/* RCTL 位定义 */
#define RCTL_EN			(1 << 1)
#define RCTL_SBP		(1 << 2)
#define RCTL_UPE		(1 << 3)
#define RCTL_MPE		(1 << 4)
#define RCTL_LPE		(1 << 5)
#define RCTL_BAM		(1 << 15)
#define RCTL_SZ_2048	(0 << 16)
#define RCTL_SECRC		(1 << 26)

#define E1000_TXD_STAT_DD	0x01 // Descriptor Done

#define SWSM_SWESMBI	(1 << 1)

#define TX_CMD_EOP  (1 << 0) // End of Packet
#define TX_CMD_IFCS (1 << 1) // Insert FCS (CRC)
#define TX_CMD_RS   (1 << 3) // Report Status

/* DMA 描述符字段需 volatile:硬件 (e1000 网卡) 通过 PCI bus master 直接
 * 写这块内存，CPU 这边不知道发生过写入.volatile 抑制编译器把字段缓存到
 * 寄存器，抑制 dead-store 消除，强制每次都重读内存。
 *
 * 关于内存屏障:x86 的强内存模型保证 DMA 写到内存的顺序(硬件按描述符
 * 字段地址递增顺序写，length/checksum/errors 都写完后才置 status.DD 位)
 * 在 CPU load 端看是有序的，所以 status 读到 DD=1 后再读 length 是安全的；
 * 跨架构若移植到 ARM，则需要在 status check 后插入 dma_rmb() 等价的屏障。
 */
struct e1000_rx_desc {
	volatile uint64_t buffer_addr;  /* CPU 写 (绑定缓冲区物理地址)，硬件读 */
	volatile uint16_t length;
	volatile uint16_t checksum;
	volatile uint8_t  status;
	volatile uint8_t  errors;
	volatile uint16_t special;
} __attribute__((packed));

#define NUM_RX_DESC 256
#define NUM_TX_DESC 256
#define E1000_TX_BUFFER_SIZE 4096

// 发送描述符结构 (DMA 共享内存，字段需 volatile，理由见 e1000_rx_desc 上方)
struct e1000_tx_desc {
	uint64_t buffer_addr;
	uint16_t length;
	uint8_t  cso;    // Checksum offset
	uint8_t  cmd;    // Command bit (EOP=1, IFCS=1, RS=1)
	volatile uint8_t status; // Status (DD=1) <- 硬件写
	uint8_t  css;    // Checksum start
	uint16_t special;
} __attribute__((packed));

typedef struct {
	// --- MMIO 基础 ---
	uint64_t mmio_base;
	uint8_t  mac[6];

	// --- 接收相关 (RX) ---
	struct e1000_rx_desc *rx_ring;      // 虚拟地址
	uint64_t             rx_ring_phys; // 物理地址
	void                 *rx_buffers[NUM_RX_DESC];
	uint32_t             rx_tail;      // 软件处理进度

	// --- 发送相关 (TX) ---
	struct e1000_tx_desc *tx_ring;      // 虚拟地址 (新增)
	uint64_t             tx_ring_phys; // 物理地址 (新增)
	void                 *tx_buffers[NUM_TX_DESC]; // (新增)
	uint32_t             tx_tail;      // 下一个待写入的索引 (新增)
	spinlock_t           tx_lock;      // Serializes TX ring ownership and buffer use.
	int (*tx)(void *nic, void *data, uint16_t len);
} e1000_driver_t;

/* 批量接收的单个条目 */
typedef struct {
	void     *data;   /* 指向 rx_buffers 中的虚拟地址(零拷贝) */
	uint16_t  len;
	uint32_t  desc_idx; /* 该包对应的描述符索引，release 时需要 */
} e1000_rx_packet_t;

#define RX_BATCH_MAX 32

/* 批量轮询：一次性收集最多 max_batch 个就绪包，不写 RDT */
int e1000_poll_packet_batch(e1000_driver_t *nic,
		e1000_rx_packet_t *out,
		int max_batch);

/* 调用者处理完这批包之后调用，统一更新 RDT，归还缓冲区给硬件 */
void e1000_rx_batch_release(e1000_driver_t *nic, e1000_rx_packet_t *batch, int count);


/* 把"收包"和"用包"两个阶段彻底解耦，网卡侧只关心"描述符指向的缓冲区是否空闲"，不关心数据有没有被处理完。
 */
/* 一条待处理报文记录 */
typedef struct {
	void     *data;      /* 报文数据的虚拟地址(零拷贝，指向原 rx_buffer) */
	uint16_t  len;
	uint64_t  buf_phys;  /* 该缓冲区的物理地址，处理完后回收要用 */
} e1000_rx_entry_t;

#define RX_QUEUE_SIZE     256   /* 待处理队列容量 */
#define RX_FREE_POOL_SIZE 256   /* 补给池：与 NUM_RX_DESC 等量，避免突发打穿走慢路径 pmm_alloc */

void e1000_rx_pipeline_init(void);
int  e1000_poll_and_enqueue(e1000_driver_t *nic);
int  e1000_rx_dequeue_packet(e1000_rx_entry_t *out);
void e1000_rx_release_buffer(e1000_rx_entry_t *entry);


void e1000_map_bar(uint64_t bar0_phys);
void e1000_reset();
void e1000_acquire_eeprom();
void e1000_read_mac(uint8_t* mac);
void e1000_check_status();
e1000_driver_t *e1000_init(void *data);
void e1000_irq_handler(void);
void e1000_wait_rx_event(void);
void e1000_init_rx(e1000_driver_t* nic);
void e1000_init_tx(e1000_driver_t *nic);
void *e1000_poll_packet(e1000_driver_t *nic, uint16_t *out_len);
void e1000_test_loop(e1000_driver_t *nic);

#endif

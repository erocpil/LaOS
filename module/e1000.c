#include "e1000.h"
#include "../kernel/pci.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../kernel/cpu.h"
#include "../kernel/heap.h"
#include "../kernel/sched.h"
#include "../kernel/string.h"
#include "../kernel/thread.h"
#include "../kernel/debug.h"
#include "../kernel/lock.h"
#include "protocol.h"
#include "../kernel/hhdm.h"
#include "../kernel/log.h"
#include "../kernel/stats.h"
#include "../kernel/timer.h"
#include "../kernel/module_param.h"

#ifdef __aarch64__
#include "../kernel/arch/aarch64/arch_cache.h"
#endif

#ifndef __aarch64__
extern void idt_register_e1000_irq_handler(void (*handler)(void));
#endif

/* 统计结构由模块持有，并通过 stats_net_register() 提供给内核监控接口。多个上下文
 * 可更新统计字段；读取采用 relaxed 原子操作，生成的快照可能包含不同时刻的数值，
 * 仅用于诊断展示，不用于同步判断或精确记账。 */
static struct e1000_stats s_stats;

#define STAT_LOAD(field) __atomic_load_n(&s_stats.field, __ATOMIC_RELAXED)
#define STAT_STORE(field, value)                                                                   \
	__atomic_store_n(&s_stats.field, (__typeof__(s_stats.field))(value), __ATOMIC_RELAXED)
#define STAT_ADD(field, value)                                                                     \
	__atomic_add_fetch(&s_stats.field, (__typeof__(s_stats.field))(value), __ATOMIC_RELAXED)
#define STAT_INC(field) STAT_ADD(field, 1)

/* 空闲策略：0 调用 schedule() 让出 CPU，1 调用 schedule_timeout(1) 休眠一个 tick。 */
static int g_idle_mode = 0;
MODULE_PARAM(g_idle_mode, INT, "0=schedule busy-wait, 1=schedule_timeout sleep");

/* 收包模式: task.conf 中配置 g_rx_mode=1~5 选择对应 loop。
 * 默认 5 (MT 双线程流水线)。 */
static int g_rx_mode = 5;
MODULE_PARAM(g_rx_mode, INT, "RX loop: 1=simple 2=batch 3=batch_swap 4=single_thr 5=mt");

static inline void e1000_idle(void)
{
	if (g_idle_mode) {
		schedule_timeout(1);
	} else {
		schedule();
	}
}

/* 定义靠后，声明在此以便前向使用 */
static void e1000_stats_snapshot(e1000_driver_t *nic);
static void e1000_hw_stats_refresh(void);

static uint64_t t0 = 0;
static uint64_t t2 = 0;
static uint64_t t3 = 0;

static uint64_t g_nic_mmio_base = 0;

static inline void e1000_write(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(g_nic_mmio_base + reg) = value;
}

static inline uint32_t e1000_read(uint32_t reg)
{
	L("g_nic_mmio_base 0x%lx reg %d", g_nic_mmio_base, reg);
	uint32_t r = *(volatile uint32_t *)(g_nic_mmio_base + reg);
	return r;
}

/* kernel_pml4 由内核导出，模块加载时通过重定位解析其地址。 */
extern uint64_t *kernel_pml4;

void e1000_map_bar(uint64_t bar0_phys)
{
#ifdef __aarch64__
	/* 当前 ARM64 启动映射采用物理地址直映，无需在此另建映射。 */
	g_nic_mmio_base = bar0_phys;
#else
	g_nic_mmio_base = NIC_VIRT_BASE;
	uint64_t flags = PTE_PRESENT | PTE_WRITABLE | (1ULL << 4) | (1ULL << 3); // 禁用缓存并启用写通。
	for (uint64_t i = 0; i < (128 * 1024); i += 4096) {
		vmm_map_global(g_nic_mmio_base + i, bar0_phys + i, flags);
	}
#endif
	L_TAG(LOG_NET, "[E1000] MMIO mapped at %p\n", g_nic_mmio_base);
}

int e1000_init_rx_ring(e1000_driver_t *nic)
{
	// 分配接收描述符环。
	void *rx_phys = pmm_alloc();
	if (!rx_phys) {
		L("[E1000] Failed to allocate RX descriptor ring\n");
		return -1;
	}
	nic->rx_ring = (struct e1000_rx_desc *)phys_to_virt((uint64_t)rx_phys);
	/* rx_ring 通过 HHDM 访问；virt_to_phys() 根据该映射换算 DMA 所需的物理地址，
	 * 无须遍历页表。 */
	nic->rx_ring_phys = (uint64_t)virt_to_phys((void *)nic->rx_ring);
	memset(nic->rx_ring, 0, 4096);

	// 为每个接收描述符分配缓冲区。
	for (int i = 0; i < NUM_RX_DESC; i++) {
		void *buf_phys = pmm_alloc();
		if (!buf_phys) {
			L("[E1000] Failed to allocate RX buffer %d\n", i);
			for (int j = 0; j < i; j++) {
				pmm_free((void *)nic->rx_ring[j].buffer_addr);
			}
			nic->rx_ring_phys = (uint64_t)NULL;
			nic->rx_ring = NULL;
			pmm_free(rx_phys);
			return -1;
		}
		nic->rx_buffers[i] = phys_to_virt((uint64_t)buf_phys);
		nic->rx_ring[i].buffer_addr = (uint64_t)buf_phys;
		nic->rx_ring[i].status = 0;
	}

#ifdef __aarch64__
	/* 对非一致性 DMA，在设备访问前将描述符和缓冲区清理至 PoC。 */
	arch_dma_sync_for_device(nic->rx_ring, NUM_RX_DESC * sizeof(struct e1000_rx_desc));
	for (int i = 0; i < NUM_RX_DESC; i++) {
		arch_dma_sync_for_device(nic->rx_buffers[i], 2048);
	}
#endif

	// 将描述符环地址和长度写入设备寄存器。
	e1000_write(REG_RDBAL, (uint32_t)nic->rx_ring_phys);
	e1000_write(REG_RDBAH, (uint32_t)(nic->rx_ring_phys >> 32));
	e1000_write(REG_RDLEN, NUM_RX_DESC * 16);

	// 初始化接收环首尾索引。
	e1000_write(REG_RDH, 0);
	// 将 RDT 指向环末尾，使设备获得全部描述符。
	e1000_write(REG_RDT, NUM_RX_DESC - 1);
	__atomic_store_n(&nic->rx_tail, 0, __ATOMIC_RELAXED);

#ifdef __x86_64__
	__asm__ volatile("sfence" ::: "memory");
#elif defined(__aarch64__)
	/* 完成缓存维护后，再通过 MMIO 发布描述符环。 */
	__asm__ volatile("dsb sy" ::: "memory");
#endif

	return 0;
}

void e1000_free_rx_ring(e1000_driver_t *nic)
{
	if (!nic || !nic->rx_ring) {
		return;
	}

	for (int i = 0; i < NUM_RX_DESC; i++) {
		if (nic->rx_ring[i].buffer_addr) {
			pmm_free((void *)nic->rx_ring[i].buffer_addr);
			nic->rx_ring[i].buffer_addr = 0;
			nic->rx_buffers[i] = NULL;
		}
	}
	pmm_free((void *)nic->rx_ring_phys);
	nic->rx_ring_phys = 0;
	nic->rx_ring = NULL;
}

void e1000_acquire_eeprom()
{
	uint32_t swsm = e1000_read(REG_SWSM);
	// 请求 EEPROM 软件访问权。
	e1000_write(REG_SWSM, swsm | SWSM_SWESMBI);

	/* SWSM 轮询设有上限，避免设备未实现该寄存器时永久阻塞。 */
	uint32_t iter = 0;
	while (!(e1000_read(REG_SWSM) & SWSM_SWESMBI)) {
		if (++iter > 10000000) {
			L("[E1000] acquire_eeprom timeout, SWSM=%x\n", e1000_read(REG_SWSM));
			return;
		}
	}
}

void e1000_reset()
{
	uint32_t ctrl = e1000_read(REG_CTRL);
	// CTRL bit 26 请求设备复位。
	e1000_write(REG_CTRL, ctrl | (1 << 26));

	// 等待设备完成复位。
	for (volatile int i = 0; i < 10000; i++) {
		/* 复位等待循环。 */
	}
}

int e1000_init_rx(e1000_driver_t *nic)
{
	// 初始化期间屏蔽设备中断。
	e1000_write(REG_IMC, 0xFFFFFFFF);

	// 初始化接收描述符环。
	if (-1 == e1000_init_rx_ring(nic)) {
		L_TAG(LOG_NET, "[E1000] e1000_init_rx_ring() failed.\n");
		return -1;
	}

	// 配置接收控制：接收广播和组播，使用 2 KiB 缓冲区，并剥离 CRC。
	uint32_t rctl = RCTL_EN | RCTL_BAM | RCTL_MPE | RCTL_SZ_2048 | RCTL_SECRC;
	e1000_write(REG_RCTL, rctl);

	L_TAG(LOG_NET, "[E1000] RX Initialized. RCTL: %x, RAL/RAH: %x:%x\n", e1000_read(REG_RCTL),
		e1000_read(REG_RAL), e1000_read(REG_RAH));

	return 0;
}

void e1000_free_rx(e1000_driver_t *drv)
{
	if (!drv) {
		return;
	}
	e1000_free_rx_ring(drv);
}

/* 扫描接收环并收集最多 max_batch 个已完成的描述符。此函数不清除完成状态，
 * 也不更新 RDT；调用方消费完数据后须调用 e1000_rx_batch_release()。无可用报文时
 * 返回 0，否则返回收集到的报文数。 */
int e1000_poll_packet_batch(e1000_driver_t *nic, e1000_rx_packet_t *out, int max_batch)
{
	int count = 0;
	uint32_t i = __atomic_load_n(&nic->rx_tail, __ATOMIC_RELAXED);

	while (count < max_batch) {
		struct e1000_rx_desc *desc = &nic->rx_ring[i];

#ifdef __aarch64__
		arch_dma_sync_for_cpu(desc, sizeof(*desc));
#endif
		/* DD 未置位表示设备尚未完成该描述符。 */
		if (!(desc->status & (1 << 0))) {
			break;
		}

		/* 记录报文信息，暂不归还描述符。 */
		out[count].data = nic->rx_buffers[i];
		out[count].len = desc->length;
		out[count].desc_idx = i;

		count++;
		i = (i + 1) % NUM_RX_DESC;
	}

	return count;
}

/* 单线程批量接收：将已完成描述符中的旧缓冲区交给调用方，并立即绑定新缓冲区，
 * 随后批量更新 RDT。该路径不使用队列或锁；返回收集到的报文数。 */
int e1000_poll_packet_batch_swap(e1000_driver_t *nic, e1000_rx_packet_t *out, int max_batch)
{
	int count = 0;
	uint32_t i = __atomic_load_n(&nic->rx_tail, __ATOMIC_RELAXED);
	uint32_t last_filled = i;
	int any = 0;

	while (count < max_batch) {
		struct e1000_rx_desc *desc = &nic->rx_ring[i];

#ifdef __aarch64__
		arch_dma_sync_for_cpu(desc, sizeof(*desc));
#endif
		if (!(desc->status & (1 << 0))) {
			break;
		}

		/* 将已接收数据的旧缓冲区交给调用方。 */
		out[count].data = nic->rx_buffers[i];
		out[count].len = desc->length;

		/* 绑定新缓冲区后才能将描述符交还设备。 */
		uint64_t new_phys = (uint64_t)pmm_alloc();
		if (!new_phys) {
			STAT_INC(rx_dropped_poolempty);
			break;
		}
		nic->rx_buffers[i] = phys_to_virt(new_phys);
		desc->buffer_addr = new_phys;
		desc->status = 0;

#ifdef __aarch64__
		/* 在设备 DMA 访问前清理新描述符和缓冲区。 */
		arch_dma_sync_for_device(desc, sizeof(*desc));
		arch_dma_sync_for_device(nic->rx_buffers[i], 2048);
#endif

		last_filled = i;
		any = 1;
		count++;
		i = (i + 1) % NUM_RX_DESC;
	}

	if (any) {
		__atomic_store_n(&nic->rx_tail, i, __ATOMIC_RELAXED);
		/* 批次里每个槛位换完缓冲区就立刻可用，
		 * 一次性写 RDT 把它们全部交还给硬件 */
		e1000_write(REG_RDT, last_filled);
	}

	return count;
}

/* 调用方处理完 batch 中的数据后调用此函数。函数清除描述符状态、推进软件尾索引，
 * 并通过一次 RDT 写入将这批描述符交还设备。调用前不得再访问对应缓冲区。 */
void e1000_rx_batch_release(e1000_driver_t *nic, e1000_rx_packet_t *batch, int count)
{
	if (count <= 0) {
		return;
	}

	for (int k = 0; k < count; k++) {
		uint32_t idx = batch[k].desc_idx;
		nic->rx_ring[idx].status = 0; /* 清状态位，准备复用 */
#ifdef __aarch64__
		arch_dma_sync_for_device(&nic->rx_ring[idx], sizeof(struct e1000_rx_desc));
#endif
	}

	/* rx_tail 推进到批次末尾的下一个位置 */
	__atomic_store_n(
		&nic->rx_tail, (batch[count - 1].desc_idx + 1) % NUM_RX_DESC, __ATOMIC_RELAXED);

	/* RDT 更新后，设备可复用此前归还的描述符。每批只写一次寄存器，减少 MMIO 操作。 */
	uint32_t rx_tail = __atomic_load_n(&nic->rx_tail, __ATOMIC_RELAXED);
	e1000_write(REG_RDT, (rx_tail + NUM_RX_DESC - 1) % NUM_RX_DESC);
}

/* 发送一个数据包，并等待设备完成对应描述符。 */
int e1000_send_packet(void *_nic, void *data, uint16_t len)
{
	e1000_driver_t *nic = (e1000_driver_t *)_nic;
	if (len > E1000_TX_BUFFER_SIZE) {
		L("[E1000] TX packet too large: %u bytes (max %u)\n", len, E1000_TX_BUFFER_SIZE);
		return -1;
	}

	/* 设备置位 DD 前，当前描述符及其缓冲区由本次发送独占。 */
	spin_lock(&nic->tx_lock);

	// 读取下一个发送描述符索引。
	uint32_t i = __atomic_load_n(&nic->tx_tail, __ATOMIC_RELAXED);
	struct e1000_tx_desc *desc = &nic->tx_ring[i];

	// 将待发送数据复制到该描述符绑定的缓冲区。
	memcpy(nic->tx_buffers[i], data, len);

	// 填写发送描述符。
	desc->length = len;
	desc->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
	desc->status = 0;

	// 推进软件索引并通知设备有新的发送请求。
	__atomic_store_n(&nic->tx_tail, (i + 1) % NUM_TX_DESC, __ATOMIC_RELAXED);
#ifdef __x86_64__
	__asm__ volatile("clflush (%0)" : : "r"(desc) : "memory");
#elif defined(__aarch64__)
	/* 设备读取描述符和负载前，清理相应缓存区域。 */
	arch_dma_sync_for_device(nic->tx_buffers[i], len);
	arch_dma_sync_for_device(desc, sizeof(*desc));
#endif
	e1000_write(REG_TDT, __atomic_load_n(&nic->tx_tail, __ATOMIC_RELAXED));

	/* 发送计数在请求提交时更新；完成超时另行统计。 */
	STAT_INC(tx_packets);
	STAT_ADD(tx_bytes, len);

	// 轮询等待设备置位 DD，或在达到上限后返回超时。
	uint32_t iter = 0;
	while (!(desc->status & 0x01)) {
		if (++iter > 10000000) {
			L("[E1000] TX DD timeout TDH=%d TDT=%d status=%x\n", e1000_read(REG_TDH),
				e1000_read(REG_TDT), desc->status);
			STAT_INC(tx_dd_timeout);
			spin_unlock(&nic->tx_lock);
			return -1;
		}
	}

	spin_unlock(&nic->tx_lock);
	return 0;
}

int e1000_init_tx(e1000_driver_t *nic)
{
	// 分配发送描述符环所需的物理连续内存。
	void *tx_phys = pmm_alloc();
	if (!tx_phys) {
		L("[E1000] Failed to allocate TX descriptor ring\n");
		return -1;
	}
	nic->tx_ring = (struct e1000_tx_desc *)phys_to_virt((uint64_t)tx_phys);
	/* 将 HHDM 虚拟地址转换为设备使用的物理地址。 */
	nic->tx_ring_phys = (uint64_t)virt_to_phys((void *)nic->tx_ring);
	L("nic->tx_ring_phys %lx\n", nic->tx_ring_phys);
	memset(nic->tx_ring, 0, 4096);

	// 为每个发送描述符预先分配一个缓冲区。
	for (int i = 0; i < NUM_TX_DESC; i++) {
		uint64_t phys = (uint64_t)pmm_alloc(); // pmm_alloc() 返回页的物理地址。
		if (!phys) {
			L("[E1000] Failed to allocate TX buffer %d\n", i);
			for (int j = 0; j < i; j++) {
				pmm_free((void *)nic->tx_ring[j].buffer_addr);
			}
			nic->tx_ring_phys = (uint64_t)NULL;
			nic->tx_ring = NULL;
			pmm_free(tx_phys);
			return -1;
		}
		nic->tx_buffers[i] = phys_to_virt(phys);
		nic->tx_ring[i].buffer_addr = phys;	    // 描述符保存设备可访问的物理地址。
		nic->tx_ring[i].status = E1000_TXD_STAT_DD; // 初始标记为可用。
	}

	// 配置发送描述符环寄存器。
	e1000_write(REG_TDBAL, (uint32_t)nic->tx_ring_phys);
	e1000_write(REG_TDBAH, (uint32_t)(nic->tx_ring_phys >> 32));
	e1000_write(REG_TDLEN, NUM_TX_DESC * 16);

	// 初始化发送环头尾索引。
	e1000_write(REG_TDH, 0);
	e1000_write(REG_TDT, 0);
	__atomic_store_n(&nic->tx_tail, 0, __ATOMIC_RELAXED);
	spin_lock_init(&nic->tx_lock);
	nic->tx = e1000_send_packet;

	// 启用发送，设置短帧填充、冲突阈值及全双工建议值。
	uint32_t tctl = (1 << 1) | (1 << 3) | (0x0F << 4) | (0x40 << 12);
	e1000_write(REG_TCTL, tctl);

	// 设置 IEEE 802.3 建议的帧间隙：IPGT=10、IPGR1=8、IPGR2=6。
	e1000_write(0x0410, 0x0060200A);

	return 0;
}

void e1000_free_tx(e1000_driver_t *drv)
{
	if (!drv || !drv->tx_ring) {
		return;
	}
	for (int i = 0; i < NUM_TX_DESC; i++) {
		if (drv->tx_ring[i].buffer_addr) {
			pmm_free((void *)drv->tx_ring[i].buffer_addr);
			drv->tx_ring[i].buffer_addr = 0;
			drv->tx_buffers[i] = NULL;
		}
	}
	pmm_free((void *)drv->tx_ring_phys);
	drv->tx_ring_phys = 0;
	drv->tx_ring = NULL;
}

/* 读取网卡 MAC 地址，写入至少 6 字节的输出缓冲区。优先读取已加载到 RAL/RAH
 * 的地址；寄存器未提供有效值时，再通过 EERD 读取 EEPROM。 */
void e1000_read_mac(uint8_t *mac)
{
	// 优先读取设备已装载到 RAL/RAH 的 MAC 地址。
	uint32_t ral = e1000_read(0x5400);
	uint32_t rah = e1000_read(0x5404);

	// RAL 无有效值时，通过 EERD 读取 EEPROM。
	if (ral != 0 && ral != 0xFFFFFFFF) {
		mac[0] = (uint8_t)(ral & 0xFF);
		mac[1] = (uint8_t)((ral >> 8) & 0xFF);
		mac[2] = (uint8_t)((ral >> 16) & 0xFF);
		mac[3] = (uint8_t)((ral >> 24) & 0xFF);
		mac[4] = (uint8_t)(rah & 0xFF);
		mac[5] = (uint8_t)((rah >> 8) & 0xFF);
		L("[E1000] MAC loaded from RAL/RAH.\n");
	} else {
		uint32_t temp;

		for (int i = 0; i < 3; i++) {
			// 设置读取启动位及 EEPROM 字地址；MAC 地址占前三个字。
			e1000_write(REG_EERD, 1 | (i << 8));

			// 等待读取完成位；设置轮询上限以避免永久阻塞。
			uint32_t iter = 0;
			while (!((temp = e1000_read(REG_EERD)) & (1 << 4))) {
				if (++iter > 10000000) {
					L("[E1000] EERD timeout word=%d EERD=%x\n", i, temp);
					return;
				}
			}

			// 从返回值中提取当前 EEPROM 字节对。
			mac[i * 2] = (uint8_t)(temp >> 16);
			mac[i * 2 + 1] = (uint8_t)(temp >> 24);
		}
	}

	L_TAG(LOG_NET, "[E1000] MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1],
		mac[2], mac[3], mac[4], mac[5]);
}

void e1000_check_status()
{
	uint32_t status = e1000_read(REG_STATUS);
	L("[E1000] Status Register: %x\n", status);

	// STATUS bit 1 表示链路已建立。
	if (status & (1 << 1)) {
		L_TAG(LOG_NET, "[E1000] Link is UP. Speed: %s\n",
			(status & (1 << 6)) ? "1000Mbps" : "10/100Mbps");
	} else {
		L_TAG(LOG_NET, "[E1000] Link is DOWN.\n");
	}
}

e1000_driver_t *e1000_alloc()
{
	e1000_driver_t *drv = NULL;

	drv = kmalloc(sizeof(e1000_driver_t));
	if (drv) {
		memset(drv, 0, sizeof(*drv));
	}

	return drv;
}

void e1000_free(e1000_driver_t *drv)
{
	kfree(drv);
}

static void arp_cache_init(void); /* 前置声明。 */

/* 中断处理程序使用的驱动实例。 */
static e1000_driver_t *g_e1000_drv = NULL;
static volatile uint64_t g_e1000_irq_count;
static volatile uint32_t g_e1000_last_icr;
static volatile uint32_t g_e1000_rx_pending;
static struct thread *g_e1000_rx_worker;

e1000_driver_t *e1000_init(void *data)
{
	(void)data;

	struct pci_device *dev = pci_find_device(E1000_VENDER, E1000_DEVICE);
	if (!dev) {
		return NULL;
	}
	/* 使用 PCI 枚举阶段解析的 BAR0；若其未配置，则采用平台提供的 MMIO 窗口。 */
	uint64_t bar0 = dev->bar_phys[0];
	if (!bar0) {
		/* QEMU virt PCI MMIO 窗口从 0x10000000 开始；为 128 KiB BAR0 分配该基址。 */
		bar0 = 0x10000000;
		pci_write32(dev->bus, dev->slot, dev->func, PCI_REG_BAR0, (uint32_t)(bar0 & ~0xF));
	}
	L_TAG(LOG_NET, "BAR0: 0x%llx (Memory Space)\n", bar0);

	e1000_map_bar(bar0);

	// 启用 PCI Bus Master，使网卡能够发起 DMA 访问。
	uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, 0x04);
	// 启用 I/O 空间、内存空间和 Bus Master。
	pci_write16(dev->bus, dev->slot, dev->func, 0x04, cmd | 0x07);

	e1000_driver_t *drv = e1000_alloc();
	if (!drv) {
		L_TAG(LOG_NET, "kmalloc() failed.");
		return NULL;
	}

	arp_cache_init();
	e1000_reset();
	e1000_acquire_eeprom();
	e1000_read_mac(drv->mac);
	e1000_check_status();
	if (-1 == e1000_init_rx(drv)) {
		L_TAG(LOG_NET, "e1000_init_rx() failed.");
		e1000_free(drv);
		return NULL;
	}
	if (-1 == e1000_init_tx(drv)) {
		L_TAG(LOG_NET, "e1000_init_tx() failed.");
		e1000_write(REG_RCTL, e1000_read(REG_RCTL) & ~RCTL_EN);
		(void)e1000_read(REG_STATUS);
		e1000_free_rx(drv);
		e1000_free(drv);
		return NULL;
	}

	g_e1000_drv = drv; /* 供中断处理程序使用。 */

#ifndef __aarch64__
	/* 将该模块实例的中断处理程序注册到 IDT 分发路径。 */
	idt_register_e1000_irq_handler(e1000_irq_handler);
#endif

	/* 接收描述符仍由轮询路径消费；当前中断路径仅验证 PCI 中断投递并记录原因。 */
	(void)e1000_read(REG_ICR);

	int irq_ok = 0;
#ifdef __aarch64__
	/* ARM64 支持 MSI 分发；MSI 不可用时再尝试 INTx。 */
	if (pci_enable_msi(dev) == 0) {
		e1000_write(REG_IMS, E1000_INT_RXT0 | E1000_INT_RXDMT0 | E1000_INT_RXO);
		L_TAG(LOG_NET, "[E1000] MSI enabled: IMS=%x\n", e1000_read(REG_IMS));
		irq_ok = 1;
	}
#endif
	if (!irq_ok && pci_enable_intx(dev) == 0) {
		e1000_write(REG_IMS, E1000_INT_RXT0 | E1000_INT_RXDMT0 | E1000_INT_RXO);
		L_TAG(LOG_NET, "[E1000] INTx enabled: IRQ %u IMS=%x\n", dev->irq_line,
			e1000_read(REG_IMS));
		irq_ok = 1;
	} else {
		if (!irq_ok) {
			L_TAG(LOG_NET, "[E1000] no IRQ route available; polling only\n");
		}
	}

	return drv;
}

/* 停止设备 DMA 与中断，释放驱动资源并注销统计信息。drv 为 e1000_init() 返回的实例。 */
void e1000_exit(e1000_driver_t *drv)
{
	if (!drv) {
		return;
	}

	/* 1. 停止设备访问 RX/TX DMA 内存 */
	uint32_t rctl = e1000_read(REG_RCTL);
	e1000_write(REG_RCTL, rctl & ~RCTL_EN);
	uint32_t tctl = e1000_read(REG_TCTL);
	e1000_write(REG_TCTL, tctl & ~TCTL_EN);

	/* 屏蔽设备的全部中断。 */
	e1000_write(REG_IMC, 0xFFFFFFFF);
	(void)e1000_read(REG_STATUS);

#ifndef __aarch64__
	idt_register_e1000_irq_handler(NULL);
#endif
	__atomic_store_n(&g_e1000_drv, NULL, __ATOMIC_RELEASE);

	/* 注销模块统计信息。 */
	stats_net_register(NULL);

	e1000_free_rx(drv);
	e1000_free_tx(drv);
	kfree(drv);

	L_TAG(LOG_NET, "[E1000] exit: RCTL=%x\n", e1000_read(REG_RCTL));
}

void e1000_handle_packet(e1000_driver_t *nic, uint8_t *packet, uint16_t len);

/* ============================================================
 * ARP 缓存：简单固定大小表，表项数 = ARP_CACHE_SIZE
 * ============================================================ */
#define ARP_CACHE_SIZE 4

static struct {
	uint32_t ip; /* 网络字节序 (LE uint32 on wire) */
	uint8_t mac[6];
	int valid;
} g_arp_cache[ARP_CACHE_SIZE];

static void arp_cache_init(void)
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		g_arp_cache[i].valid = 0;
	}
}

static void arp_cache_update(uint32_t ip, const uint8_t *mac)
{
	int slot = -1;
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
			slot = i; /* 更新已有条目 */
			break;
		}
		if (!g_arp_cache[i].valid && slot < 0) {
			slot = i; /* 首个空闲槽 */
		}
	}
	if (slot < 0) {
		slot = 0; /* 表满→覆盖最旧 */
	}
	g_arp_cache[slot].ip = ip;
	memcpy(g_arp_cache[slot].mac, mac, 6);
	g_arp_cache[slot].valid = 1;
}

static const uint8_t *arp_cache_lookup(uint32_t ip)
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
			return g_arp_cache[i].mac;
		}
	}
	return NULL;
}

void e1000_handle_packet(e1000_driver_t *nic, uint8_t *packet, uint16_t len)
{
#ifdef __aarch64__
	/* 解析设备写入的报文前，使 CPU 缓存与设备写入的数据保持一致。 */
	arch_dma_sync_for_cpu(packet, len);
#endif

	if (len < sizeof(struct eth_header)) {
		return;
	}

	struct eth_header *eth = (struct eth_header *)packet;
	uint16_t type = ntoh16(eth->type);

	/* 处理 ARP 报文。 */
	if (type == 0x0806) {
		STAT_INC(arp_rx);
		struct arp_packet *arp = (struct arp_packet *)(packet + sizeof(struct eth_header));
		uint16_t opcode = ntoh16(arp->opcode);

		/* 对 192.168.56.0/24 子网内地址的 ARP 请求发送应答。 */
		if (opcode == 1 && (arp->dest_ip & 0x00FFFFFF) == 0x0038A8C0) {
			send_arp_reply(nic, eth, arp);
			STAT_INC(arp_replies);
		}
		/* 收到 ARP 应答时，缓存发送方的 MAC 与 IP 地址映射。 */
		else if (opcode == 2) {
			/* 仅缓存本地子网相关的条目。 */
			arp_cache_update(arp->src_ip, arp->src_mac);
			L_TAG(LOG_NET,
				"[ARP] reply from %d.%d.%d.%d → "
				"%02x:%02x:%02x:%02x:%02x:%02x\n",
				(int)(arp->src_ip & 0xFF), (int)((arp->src_ip >> 8) & 0xFF),
				(int)((arp->src_ip >> 16) & 0xFF),
				(int)((arp->src_ip >> 24) & 0xFF), arp->src_mac[0], arp->src_mac[1],
				arp->src_mac[2], arp->src_mac[3], arp->src_mac[4], arp->src_mac[5]);
		}
	} else if (0x0800 == type) {
		struct ip_header *ip = (struct ip_header *)(packet + sizeof(struct eth_header));
		if (ip->proto == 1) { // ICMP。
			struct icmp_header *icmp =
				(struct icmp_header *)(packet + sizeof(struct eth_header) +
					sizeof(struct ip_header));

			if (icmp->type == 8) { // Echo Request。
				STAT_INC(icmp_echo_rx);
				int ip_total_len = ntoh16(ip->len);
				int payload_len = ip_total_len - sizeof(struct ip_header) -
					sizeof(struct icmp_header);
				send_icmp_reply(nic, eth, ip, icmp, payload_len);
				STAT_INC(icmp_echo_reply);
			} else if (icmp->type == 0) { // Echo Reply。
				L_TAG(LOG_NET,
					"[ICMP] Echo Reply from %d.%d.%d.%d "
					"seq=%u id=0x%x len=%u\n",
					(int)(ip->src_ip & 0xFF), (int)((ip->src_ip >> 8) & 0xFF),
					(int)((ip->src_ip >> 16) & 0xFF),
					(int)((ip->src_ip >> 24) & 0xFF), ntoh16(icmp->seq),
					ntoh16(icmp->id), ntoh16(ip->len));
			}
		} else {
			STAT_INC(rx_dropped_unknown_proto);
		}
	} else {
		STAT_INC(rx_dropped_unknown_ether);
	}
}

void e1000_send_test_broadcast(e1000_driver_t *nic)
{
	uint8_t dummy[64];
	memset(dummy, 0xff, 6);
	memcpy(dummy + 6, nic->mac, 6);
	// memset(dummy + 6, 0xcc, 6); // SRC
	dummy[12] = 0x08;
	dummy[13] = 0x00; // IPv4 类型字段。
	memset(dummy + 14, 0xAB, 50); // 测试负载。
	e1000_send_packet(nic, dummy, 64);
}

void *e1000_poll_packet(e1000_driver_t *nic, uint16_t *out_len)
{
	uint32_t i = __atomic_load_n(&nic->rx_tail, __ATOMIC_RELAXED);
	struct e1000_rx_desc *desc = &nic->rx_ring[i];

#ifdef __aarch64__
	arch_dma_sync_for_cpu(desc, sizeof(*desc));
#endif

	/* 检查设备写回的 DD 位。AArch64 在此之前执行 DMA 缓存同步；x86
	 * 使用一致性 DMA 映射。volatile 字段访问不替代设备协议或缓存维护。 */
	if (!(desc->status & (1 << 0))) {
		return NULL;
	}

	t0 = rdtsc();
	/* 设备置位 DD 后，读取该描述符中的长度和缓冲区数据。 */
	*out_len = desc->length;
	void *buffer_virt = nic->rx_buffers[i];

#ifdef __aarch64__
	arch_dma_sync_for_cpu(buffer_virt, *out_len);
#endif

	/* 清除完成位并将描述符归还给设备。 */
	desc->status = 0;

	/* 更新软件环索引。 */
	__atomic_store_n(&nic->rx_tail, (i + 1) % NUM_RX_DESC, __ATOMIC_RELAXED);

	/* 更新硬件尾指针，使设备可重新使用该描述符。 */
	e1000_write(REG_RDT, i);

	return buffer_virt;
}

/* ============================================================
 * RX 工作循环对比基线
 *
 * 5 个 e1000_test_loop_* 函数是同一个收包问题的不同处理方式，
 * 保留是为了对比延迟/吞吐特性，_start 选其中一个跑。
 *
 *   函数                  并发模型           批量    缓冲区分配
 *   --------------------- ------------------ ------- ----------------
 *   _test_loop            单线程，逐包         无     原地复用 (零拷贝读出后立刻还描述符)
 *   _test_loop_batch      单线程，批量         32     原地复用
 *   _test_loop_batch_swap 单线程，批量+换页    32     从 pool 换新页(零拷贝交付上层)
 *   _test_loop_st         单线程，流水线       16     pool + queue (生产消费同线程)
 *   _test_loop_mt         双线程，流水线       16     pool + queue (poll/process 分核)
 *
 * 默认走 _test_loop_mt (见 _start)，其他作为基线参考保留。
 * ============================================================ */
void e1000_test_loop(e1000_driver_t *nic)
{
	struct cpu_context *ctx = cpu_get_ctx();
	L_TAG(LOG_NET, "[E1000] Start Polling on CPU %d... \n", ctx->id);
	uint16_t len = 0;
	while (1) {
		void *packet = e1000_poll_packet(nic, &len);
		if (packet) {
			t2 = rdtsc();
			e1000_handle_packet(nic, packet, len);
			t3 = rdtsc();
			// L("RX: %lu, PROC: %lu", t1 - t0, t3 - t2);
			// e1000_send_test_broadcast(nic);
		} else {
			// schedule();
			// schedule_timeout(1);
		}
		// __asm__ volatile("pause");
		// schedule_timeout(1000);
		// L();
	}
}

void e1000_test_loop_batch(e1000_driver_t *nic)
{
	struct cpu_context *ctx = cpu_get_ctx();
	L_TAG(LOG_NET, "[E1000] Start Polling on CPU %d... \n", ctx->id);

	e1000_rx_packet_t batch[RX_BATCH_MAX];

	while (1) {
		int n = e1000_poll_packet_batch(nic, batch, RX_BATCH_MAX);

		if (n > 0) {
			t2 = rdtsc();

			for (int k = 0; k < n; k++) {
				e1000_handle_packet(nic, (uint8_t *)batch[k].data, batch[k].len);
			}

			t3 = rdtsc();
			// L("RX batch: %d packets, PROC: %lu", n, t3 - t2);

			/* 这一批全部处理完，统一归还给硬件 */
			e1000_rx_batch_release(nic, batch, n);
		} else {
			e1000_idle();
		}
	}
}

void e1000_test_loop_batch_swap(e1000_driver_t *nic)
{
	struct cpu_context *ctx = cpu_get_ctx();
	L_TAG(LOG_NET, "[E1000] Start Polling on CPU %d... \n", ctx->id);

	e1000_rx_packet_t batch[RX_BATCH_MAX];

	while (1) {
		int n = e1000_poll_packet_batch_swap(nic, batch, RX_BATCH_MAX);

		if (n > 0) {
			t2 = rdtsc();

			for (int k = 0; k < n; k++) {
				e1000_handle_packet(nic, (uint8_t *)batch[k].data, batch[k].len);
			}

			t3 = rdtsc();
			// L("RX batch: %d packets, PROC: %lu", n, t3 - t2);

		} else {
			schedule();
		}
	}
}

typedef struct {
	uint64_t phys[RX_FREE_POOL_SIZE];
	uint32_t count;
	spinlock_t lock;
} e1000_free_pool_t;

static e1000_free_pool_t g_rx_free_pool;

/* 预先分配 target_count 个物理页并加入空闲池。
 *
 * 锁说明：本驱动 free_pool 与 rx_queue 都用普通 spin_lock，不用 irqsave.
 * 因为唯一访问者是 e1000_rx_poll_thread / e1000_rx_process_thread 两个线程
 * 上下文，NIC IRQ 被 IMC=0xFFFFFFFF 全屏蔽 (见 e1000_init_rx)，没有中断路径
 * 会保存/恢复中断状态，没必要为这里增加该开销。
 * 若以后开 NIC 中断收包，把这几处改回 spin_lock_irqsave 即可。 */
static void rx_free_pool_refill(e1000_free_pool_t *pool, int target_count)
{
	spin_lock(&pool->lock);
	while (pool->count < (uint32_t)target_count && pool->count < RX_FREE_POOL_SIZE) {
		uint64_t page = (uint64_t)pmm_alloc();
		if (!page) {
			break;
		}
		pool->phys[pool->count++] = page;
	}
	spin_unlock(&pool->lock);
}

/* 从空闲池取页；池为空时回退到物理页分配器。 */
static uint64_t rx_free_pool_get(e1000_free_pool_t *pool)
{
	uint64_t phys = 0;

	spin_lock(&pool->lock);
	if (pool->count > 0) {
		phys = pool->phys[--pool->count];
	}
	spin_unlock(&pool->lock);

	if (!phys) {
		L("rx free pool empty, falling back to pmm_alloc()");
		STAT_INC(rx_dropped_poolempty);
		phys = (uint64_t)pmm_alloc();
	}

	return phys;
}

/* 报文处理完成后归还缓冲区；空闲池已满时将页面释放给物理内存管理器。 */
static void rx_free_pool_put(e1000_free_pool_t *pool, uint64_t phys)
{
	spin_lock(&pool->lock);

	if (pool->count < RX_FREE_POOL_SIZE) {
		pool->phys[pool->count++] = phys;
		spin_unlock(&pool->lock);
		return;
	}

	spin_unlock(&pool->lock);

	pmm_free((void *)phys);
}

typedef struct {
	e1000_rx_entry_t entries[RX_QUEUE_SIZE];
	/* 消费者线程从此处取出报文。 */
	uint32_t head;
	/* 轮询生产者在此处加入报文。 */
	uint32_t tail;
	uint32_t count;
	spinlock_t lock;
} e1000_rx_queue_t;

static e1000_rx_queue_t g_rx_queue;

static int rx_queue_push(e1000_rx_queue_t *q, e1000_rx_entry_t *entry)
{
	int ok = 0;

	spin_lock(&q->lock);

	if (q->count < RX_QUEUE_SIZE) {
		q->entries[q->tail] = *entry;
		q->tail = (q->tail + 1) % RX_QUEUE_SIZE;
		q->count++;
		if (q->count > STAT_LOAD(rx_queue_hwm)) {
			STAT_STORE(rx_queue_hwm, q->count);
		}
		ok = 1;
	}

	spin_unlock(&q->lock);

	return ok;
}

static int rx_queue_pop(e1000_rx_queue_t *q, e1000_rx_entry_t *out)
{
	int ok = 0;

	spin_lock(&q->lock);

	if (q->count > 0) {
		*out = q->entries[q->head];
		q->head = (q->head + 1) % RX_QUEUE_SIZE;
		q->count--;
		ok = 1;
	}

	spin_unlock(&q->lock);

	return ok;
}

/* 扫描已完成的接收描述符，为其绑定新缓冲区，并将旧缓冲区加入处理队列。
 * 处理线程消费报文后须调用 e1000_rx_release_buffer() 归还缓冲区。 */
int e1000_poll_and_enqueue(e1000_driver_t *nic)
{
	int n = 0;
	uint32_t i = __atomic_load_n(&nic->rx_tail, __ATOMIC_RELAXED);
	uint32_t last_filled = i;

	while (1) {
		struct e1000_rx_desc *desc = &nic->rx_ring[i];

#ifdef __aarch64__
		arch_dma_sync_for_cpu(desc, sizeof(*desc));
#endif
		if (!(desc->status & (1 << 0))) {
			break; /* 没有更多就绪的包 */
		}

		/* 1. 准备一个干净的新缓冲区 */
		uint64_t new_phys = rx_free_pool_get(&g_rx_free_pool);
		void *new_virt = phys_to_virt(new_phys);

		/* 将含有已接收报文的旧缓冲区加入处理队列。 */
		e1000_rx_entry_t entry;
		entry.data = nic->rx_buffers[i];
		entry.len = desc->length;
		entry.buf_phys = desc->buffer_addr;

		if (!rx_queue_push(&g_rx_queue, &entry)) {
			/* 队列已满时归还新缓冲区并停止扫描。描述符仍保留 DD 状态，报文数据
			 * 未丢失，待队列有空位后可继续处理。 */
			rx_free_pool_put(&g_rx_free_pool, new_phys);
			STAT_INC(rx_dropped_qfull);
			L("rx queue full, pausing at desc %u", i);
			break;
		}

		/* 将新缓冲区绑定到该描述符。 */
		nic->rx_buffers[i] = new_virt;
		desc->buffer_addr = new_phys;
		desc->status = 0; /* 清除完成状态，准备归还设备。 */

#ifdef __aarch64__
		arch_dma_sync_for_device(desc, sizeof(*desc));
		arch_dma_sync_for_device(new_virt, 2048);
#endif

		/* 统计成功加入接收队列的报文。 */
		STAT_INC(rx_packets);
		STAT_ADD(rx_bytes, entry.len);

		last_filled = i;
		n++;
		i = (i + 1) % NUM_RX_DESC;
	}

	if (n > 0) {
		__atomic_store_n(&nic->rx_tail, i, __ATOMIC_RELAXED);
		/* 一次更新 RDT，发布本批已归还的描述符。 */
		e1000_write(REG_RDT, last_filled);
	}

	return n;
}

int e1000_rx_dequeue_packet(e1000_rx_entry_t *out)
{
	return rx_queue_pop(&g_rx_queue, out);
}

/* 报文处理完成后归还其缓冲区，否则空闲池会逐渐耗尽。 */
void e1000_rx_release_buffer(e1000_rx_entry_t *entry)
{
	rx_free_pool_put(&g_rx_free_pool, entry->buf_phys);
}

void e1000_rx_pipeline_init(void)
{
	/* 初始化只执行一次。重复清零会破坏队列、自旋锁及已入队条目的状态。 */
	static int inited = 0;
	if (inited) {
		return;
	}
	inited = 1;

	memset(&g_rx_free_pool, 0, sizeof(g_rx_free_pool));
	memset(&g_rx_queue, 0, sizeof(g_rx_queue));
	rx_free_pool_refill(&g_rx_free_pool, RX_FREE_POOL_SIZE);
}

void e1000_test_loop_st(e1000_driver_t *nic)
{
	e1000_rx_pipeline_init();

	e1000_rx_entry_t entry;

	while (1) {
		/* 尽快将已接收报文转入队列，使设备能够继续复用描述符。 */
		int n = e1000_poll_and_enqueue(nic);

		/* 在消费者线程中处理报文，避免处理耗时阻塞接收路径。 */
		int processed = 0;
		while (processed < RX_BATCH_MAX && e1000_rx_dequeue_packet(&entry)) {
			e1000_handle_packet(nic, (uint8_t *)entry.data, entry.len);
			e1000_rx_release_buffer(&entry);
			processed++;
		}

		if (n == 0 && processed == 0) {
			e1000_idle();
		}
	}
}

/* 线程 A:只管收，不管处理 */
void e1000_rx_poll_thread(void *data)
{
	e1000_driver_t *nic = NULL;

	nic = (e1000_driver_t *)data;
	while (1) {
		int n = e1000_poll_and_enqueue(nic);
		if (n == 0) {
			e1000_idle();
		}
	}
}

/* 线程 B:只管处理，处理慢点也无所谓 */
void e1000_rx_process_thread(void *data)
{
	e1000_driver_t *nic = NULL;

	nic = (e1000_driver_t *)data;
	e1000_rx_entry_t entry;
	uint64_t hw_last_tick = atomic64_read(&timer_ticks);

	while (1) {
		/* 批量处理 RX_BATCH_MAX 包再让出，与 _test_loop_st 行为对齐，
		 * 减少单包一对 lock acquire/release 的开销 */
		int processed = 0;
		while (processed < RX_BATCH_MAX && e1000_rx_dequeue_packet(&entry)) {
			e1000_handle_packet(nic, (uint8_t *)entry.data, entry.len);
			e1000_rx_release_buffer(&entry);
			processed++;
		}

		if (processed > 0) {
			STAT_ADD(rx_processed, processed);
			/* 每批处理后刷新环索引、队列和缓冲池计数及链路状态。 */
			e1000_stats_snapshot(nic);
		}

		/* 每 50 个 tick（约 500 ms）刷新一次硬件计数器。即使没有处理报文也须检查，
		 * 因为无缓冲区、CRC 错误和冲突等计数会在链路空闲时继续累积。 */
		if (atomic64_read(&timer_ticks) - hw_last_tick >= TIMER_HZ / 2) {
			e1000_hw_stats_refresh();
			hw_last_tick = atomic64_read(&timer_ticks);
		}

		if (processed == 0) {
			e1000_idle();
		}
	}
}

/* 从驱动、接收队列和空闲页池读取实时状态并写入统计结构。此类字段表示当前状态，
 * 不应按增量累计；监控读取到的值可能略有延迟。 */
static void e1000_stats_snapshot(e1000_driver_t *nic)
{
	if (!nic) {
		return;
	}
	STAT_STORE(rx_ring_tail, __atomic_load_n(&nic->rx_tail, __ATOMIC_RELAXED));
	STAT_STORE(tx_ring_tail, __atomic_load_n(&nic->tx_tail, __ATOMIC_RELAXED));
	STAT_STORE(rx_ring_head, e1000_read(REG_RDH));
	STAT_STORE(tx_ring_head, e1000_read(REG_TDH));
	spin_lock(&g_rx_queue.lock);
	uint32_t queue_count = g_rx_queue.count;
	spin_unlock(&g_rx_queue.lock);
	STAT_STORE(rx_queue_count, queue_count);
	spin_lock(&g_rx_free_pool.lock);
	uint32_t free_pool_count = g_rx_free_pool.count;
	spin_unlock(&g_rx_free_pool.lock);
	STAT_STORE(free_pool_count, free_pool_count);

	uint32_t status = e1000_read(REG_STATUS);
	STAT_STORE(link_up, (status & (1u << 1)) ? 1 : 0);
	STAT_STORE(link_speed, (status & (1u << 6)) ? 1000 : 100);
	for (int i = 0; i < 6; i++) {
		__atomic_store_n(&s_stats.mac[i], nic->mac[i], __ATOMIC_RELAXED);
	}
	STAT_STORE(rx_mode, g_rx_mode);
	STAT_STORE(idle_mode, g_idle_mode);
}

/* 读取硬件读清零计数器，并将增量累加到 s_stats.hw_*。本函数仅由
 * rx_process_thread() 调用，约每 500 ms 执行一次，以避免并发读取造成计数丢失，
 * 并降低 32 位计数器在高流量下溢出的风险。GORCL/GORCH 和 GOTCL/GOTCH 必须先读
 * 低位，再读高位；读取低位会锁存高位，随后读取高位并清除整组计数。 */
static void e1000_hw_stats_refresh(void)
{
	STAT_ADD(hw_crcerrs, e1000_read(REG_CRCERRS));
	STAT_ADD(hw_mpc, e1000_read(REG_MPC));
	STAT_ADD(hw_colc, e1000_read(REG_COLC));
	STAT_ADD(hw_gprc, e1000_read(REG_GPRC));
	STAT_ADD(hw_gptc, e1000_read(REG_GPTC));

	/* 读取 64 位字节计数时，必须先读低 32 位。 */
	uint32_t gorcl = e1000_read(REG_GORCL);
	uint32_t gorch = e1000_read(REG_GORCH);
	STAT_ADD(hw_gorc, ((uint64_t)gorch << 32) | gorcl);

	uint32_t gotcl = e1000_read(REG_GOTCL);
	uint32_t gotch = e1000_read(REG_GOTCH);
	STAT_ADD(hw_gotc, ((uint64_t)gotch << 32) | gotcl);

	STAT_ADD(hw_rnbc, e1000_read(REG_RNBC));
	STAT_ADD(hw_tpr, e1000_read(REG_TPR));
	STAT_ADD(hw_tpt, e1000_read(REG_TPT));
}

void e1000_test_loop_mt(void *data)
{
	e1000_rx_pipeline_init();

	int cpu = cpu_get_ctx()->id;
	struct thread *t = thread_create_on(e1000_rx_process_thread, data, cpu < 0 ? 0 : cpu);
	thread_set_name(t, "e1000_proc");
	cpu_enqueue(-1, t);
	// thread_set_name(get_current(), "e1000_rx");
	e1000_rx_poll_thread((e1000_driver_t *)data);
}

/* 发送 ARP 请求探测网关 MAC 地址。gateway_ip 使用网络字节序；该函数用于验证
 * 数据包发送和接收路径。 */
void e1000_probe_gateway(e1000_driver_t *nic, uint32_t gateway_ip)
{
	uint8_t frame[64];
	memset(frame, 0, sizeof(frame));

	/* 目的地址为广播 MAC，源地址为本机 MAC，帧类型为 ARP。 */
	memset(frame, 0xFF, 6);
	memcpy(frame + 6, nic->mac, 6);
	frame[12] = 0x08;
	frame[13] = 0x06;

	/* ARP 首部紧随 14 字节以太网头。 */
	struct arp_packet *arp = (struct arp_packet *)(frame + sizeof(struct eth_header));
	arp->hw_type = hton16(1);
	arp->proto_type = hton16(0x0800);
	arp->hw_addr_len = 6;
	arp->proto_addr_len = 4;
	arp->opcode = hton16(1); /* 请求。 */
	memcpy(arp->src_mac, nic->mac, 6);
	arp->src_ip = hton32(0xC0A8380F); /* 192.168.56.15 */
	memset(arp->dest_mac, 0, 6);
	arp->dest_ip = hton32(gateway_ip);

	e1000_send_packet(nic, frame, sizeof(struct eth_header) + sizeof(struct arp_packet));
	L_TAG(LOG_NET, "[E1000] ARP probe sent to %d.%d.%d.%d\n", (int)((gateway_ip >> 24) & 0xFF),
		(int)((gateway_ip >> 16) & 0xFF), (int)((gateway_ip >> 8) & 0xFF),
		(int)(gateway_ip & 0xFF));
}

/* 向网关发送 ICMP Echo Request。
 *
 * 前提：ARP 缓存中已有网关条目（先调 e1000_probe_gateway + 收到 reply）。
 * 返回 0 成功，-1 无 ARP 条目，-2 TX 失败。
 */
int e1000_ping_gateway(e1000_driver_t *nic, uint32_t gateway_ip)
{
	const uint8_t *gw_mac = arp_cache_lookup(hton32(gateway_ip));
	if (!gw_mac) {
		L_TAG(LOG_NET, "[ICMP] no ARP entry for gateway, skip ping\n");
		return -1;
	}

	/* 使用固定的 ICMP 负载。 */
	static const char *payload = "LaOS ping!";
	int pay_len = 12;

	uint8_t frame[256];
	memset(frame, 0, sizeof(frame));

	/* 以太网头。 */
	struct eth_header *eth = (struct eth_header *)frame;
	memcpy(eth->dest, gw_mac, 6);
	memcpy(eth->src, nic->mac, 6);
	eth->type = hton16(0x0800);

	/* IPv4 首部。 */
	struct ip_header *ip = (struct ip_header *)(frame + sizeof(struct eth_header));
	ip->version_ihl = 0x45; /* IPv4，首部长度为 20 字节。 */
	ip->tos = 0;
	ip->len = hton16(sizeof(struct ip_header) + sizeof(struct icmp_header) + pay_len);
	ip->id = hton16(0x0001);
	ip->flags_offset = 0;
	ip->ttl = 64;
	ip->proto = 1;			 /* ICMP。 */
	ip->src_ip = hton32(0xC0A8380F); /* 192.168.56.15 */
	ip->dest_ip = hton32(gateway_ip);
	ip->checksum = 0;
	ip->checksum = net_checksum(ip, sizeof(struct ip_header));

	/* ICMP 首部。 */
	struct icmp_header *icmp = (struct icmp_header *)(frame + sizeof(struct eth_header) +
		sizeof(struct ip_header));
	icmp->type = 8; /* Echo Request。 */
	icmp->code = 0;
	icmp->id = hton16(0x1234);
	icmp->seq = hton16(1);
	memcpy((uint8_t *)icmp + sizeof(struct icmp_header), payload, pay_len);
	icmp->checksum = 0;
	icmp->checksum = net_checksum(icmp, sizeof(struct icmp_header) + pay_len);

	int total_len = sizeof(struct eth_header) + sizeof(struct ip_header) +
		sizeof(struct icmp_header) + pay_len;
	int ret = e1000_send_packet(nic, frame, total_len);
	if (ret == 0) {
		L_TAG(LOG_NET, "[ICMP] Echo Request sent to %d.%d.%d.%d\n",
			(int)((gateway_ip >> 24) & 0xFF), (int)((gateway_ip >> 16) & 0xFF),
			(int)((gateway_ip >> 8) & 0xFF), (int)(gateway_ip & 0xFF));
	}
	return ret;
}

/* 模块入口 laos_module_entry(argc, argv)。
 *
 * MODULE_PARAM 声明的变量（如 g_idle_mode）由内核在加载后、本函数执行前
 * 根据 task.conf 的 key=value 对自动写入，无需手动解析。
 *
 * 返回时由 thread_entry_point 调用 thread_exit(code)。 */
int laos_module_entry(int argc, char *argv[])
{
	e1000_driver_t *drv;

	(void)argc;
	(void)argv;
	/* g_idle_mode 已由内核根据 task.conf 参数完成初始化。 */

	drv = e1000_init(NULL);

	if (drv) {
		/* 向内核统计接口注册本模块的统计结构。 */
		stats_net_register(&s_stats);
		e1000_stats_snapshot(drv);

		/* g_rx_mode 由 task.conf 配置，默认选择多线程接收流水线。 */
		switch (g_rx_mode) {
			case 1:
				e1000_test_loop(drv);
				break;
			case 2:
				e1000_test_loop_batch(drv);
				break;
			case 3:
				e1000_test_loop_batch_swap(drv);
				break;
			case 4:
				e1000_test_loop_st(drv);
				break;
			default:
				e1000_test_loop_mt(drv);
				break;
		}

		e1000_exit(drv);
	}

	L("E1000 thread exiting");
	return 0;
}

/* 中断处理程序仅确认设备中断、记录中断原因并唤醒接收线程；接收环仍由线程上下文
 * 独占访问。 */
void e1000_irq_handler(void)
{
	if (!g_e1000_drv) {
		return;
	}
	uint32_t cause = e1000_read(REG_ICR);
	if (!cause) {
		return;
	}
	g_e1000_last_icr = cause;
	uint64_t count = ++g_e1000_irq_count;
	if (cause & (E1000_INT_RXT0 | E1000_INT_RXDMT0 | E1000_INT_RXO)) {
		__atomic_store_n(&g_e1000_rx_pending, 1, __ATOMIC_RELEASE);
	}

	struct thread *worker = __atomic_load_n(&g_e1000_rx_worker, __ATOMIC_ACQUIRE);
	if (worker && thread_get_status(worker) == THREAD_BLOCKED) {
		thread_set_status(worker, THREAD_READY);
		struct cpu_context *target = g_cpu_contexts[worker->target_cpu];
		if (target) {
			target->need_resched = 1;
		}
	}
	if (count <= 8) {
		kprintf("[e1000] IRQ #%llu ICR=0x%x\n", count, cause);
	}
}

void e1000_wait_rx_event(void)
{
	struct thread *current = get_current();
	__atomic_store_n(&g_e1000_rx_worker, current, __ATOMIC_RELEASE);

	for (;;) {
		uint64_t irq_flags = save_and_disable_interrupts();
		if (__atomic_exchange_n(&g_e1000_rx_pending, 0, __ATOMIC_ACQ_REL)) {
			restore_interrupts(irq_flags);
			return;
		}

		/* 在最终检查和转入 BLOCKED 状态期间屏蔽本地中断，避免丢失唤醒：中断要么在
		 * schedule() 前唤醒线程，要么在线程阻塞后触发唤醒。 */
		thread_set_status(current, THREAD_BLOCKED);
		current->sleep_times++;
		restore_interrupts(irq_flags);
		schedule();
	}
}

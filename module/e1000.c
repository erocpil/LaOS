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
#include "../kernel/debug.h"
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

/*
 * s_stats : 本模块的埋点计数总容器，路径 γ:
 * 定义在 module 侧，_start 里调 stats_net_register(&s_stats) 把地址交给 kernel，
 * kernel/stats.c 的 stats_net() 拿到指针后直接读。
 *
 * 竞态:writer 分散在 poll_thread / process_thread / send_packet 等多个上下文，
 * 每个字段是 uint64_t/uint32_t 加法，x86_64 天然 8 字节对齐单写单原子；
 * reader(monitor thread)读到中间态无关正确性：展示层允许 skew.
 * 唯一需要额外维护的是 rx_queue_hwm，写侧本来就在 spin_lock 保护范围内，
 * 顺手更新即可。
 */
static struct e1000_stats s_stats;

/* 空闲策略: 0 = schedule() 忙等, 1 = schedule_timeout(1) 休眠 */
static int g_idle_mode = 0;
MODULE_PARAM(g_idle_mode, INT, "0=schedule busy-wait, 1=schedule_timeout sleep");

/* 收包模式: task.conf 中配置 g_rx_mode=1~5 选择对应 loop。
 * 默认 5 (MT 双线程流水线)。 */
static int g_rx_mode = 5;
MODULE_PARAM(g_rx_mode, INT, "RX loop: 1=simple 2=batch 3=batch_swap 4=single_thr 5=mt");

static inline void e1000_idle(void)
{
	if (g_idle_mode)
		schedule_timeout(1);
	else
		schedule();
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
	*(volatile uint32_t*)(g_nic_mmio_base + reg) = value;
}

static inline uint32_t e1000_read(uint32_t reg)
{
	L("g_nic_mmio_base 0x%lx reg %d", g_nic_mmio_base, reg);
	uint32_t r = *(volatile uint32_t*)(g_nic_mmio_base + reg);
	return r;
}

/* 告诉编译器这些符号在外部，链接时由重定位填入真实地址。
 * hhdm_offset() 通过 hhdm.h 的 inline 调用 + ksym lookup 接入。 */
extern uint64_t* kernel_pml4;

void e1000_map_bar(uint64_t bar0_phys)
{
#ifdef __aarch64__
	/* ARM64 identity mapping: physical == virtual, no remap needed */
	g_nic_mmio_base = bar0_phys;
#else
	g_nic_mmio_base = NIC_VIRT_BASE;
	uint64_t flags = PTE_PRESENT | PTE_WRITABLE | (1ULL << 4) | (1ULL << 3); // PCD + PWT
	for (uint64_t i = 0; i < (128 * 1024); i += 4096) {
		vmm_map_global(g_nic_mmio_base + i, bar0_phys + i, flags);
	}
#endif
	L_TAG(LOG_NET, "[E1000] MMIO mapped at %p\n", g_nic_mmio_base);
}

void e1000_init_rx_ring(e1000_driver_t *nic)
{
	// 1. 分配环内存
	void *rx_phys = pmm_alloc();
	if (!rx_phys) {
		L("[E1000] Failed to allocate RX descriptor ring\n");
		return;
	}
	nic->rx_ring = (struct e1000_rx_desc *)phys_to_virt((uint64_t)rx_phys);
	/* rx_ring 是 pmm_alloc()+hhdm_offset 得来的 HHDM 虚地址，hhdm 是
	 * 全物理内存的线性映射，直接减偏移即得物理地址，无需走页表查询。
	 * 用 vmm_get_phys 需要逐级 PML4/PDPT/PD/PT 查表，对启动期热路径
	 * 来说没必要，且依赖当前页表根正确映射 HHDM。*/
	nic->rx_ring_phys = (uint64_t)virt_to_phys((void*)nic->rx_ring);
	memset(nic->rx_ring, 0, 4096);

	// 2. 分配缓冲区
	for (int i = 0; i < NUM_RX_DESC; i++) {
		void* buf_phys = pmm_alloc();
		if (!buf_phys) {
			L("[E1000] Failed to allocate RX buffer %d\n", i);
			return;
		}
		nic->rx_buffers[i] = phys_to_virt((uint64_t)buf_phys);
		nic->rx_ring[i].buffer_addr = (uint64_t)buf_phys;
		nic->rx_ring[i].status = 0;
	}

#ifdef __aarch64__
	/* Non-coherent DMA: clean descriptors + buffers to PoC before
	 * the device starts reading them. */
	arch_dma_sync_for_device(nic->rx_ring,
		NUM_RX_DESC * sizeof(struct e1000_rx_desc));
	for (int i = 0; i < NUM_RX_DESC; i++)
		arch_dma_sync_for_device(nic->rx_buffers[i], 2048);
#endif

	// 3. 告知硬件
	e1000_write(REG_RDBAL, (uint32_t)nic->rx_ring_phys);
	e1000_write(REG_RDBAH, (uint32_t)(nic->rx_ring_phys >> 32));
	e1000_write(REG_RDLEN, NUM_RX_DESC * 16);

	// 4. 初始化指针
	e1000_write(REG_RDH, 0);
	e1000_write(REG_RDT, NUM_RX_DESC - 1); // 极其关键：硬件在 RDH==RDT 时认为环满，所以要设为最后一位
	nic->rx_tail = 0;

#ifdef __x86_64__
	__asm__ volatile ("sfence" ::: "memory");
#elif defined(__aarch64__)
	/* Complete cache maintenance before publishing the ring to MMIO. */
	__asm__ volatile ("dsb sy" ::: "memory");
#endif
}

void e1000_acquire_eeprom()
{
	uint32_t swsm = e1000_read(REG_SWSM);
	// 尝试获取信号量
	e1000_write(REG_SWSM, swsm | SWSM_SWESMBI);

	/* 轮询直到拿到锁。原版裸 while 在 QEMU 模型不实现 SWSM 时整核卡死，
	 * 加 iter 上限保护(典型 3GHz x ~100ms ~= 3e8 cycle，1e7 次空 MMIO 读
	 * 远超合理等待，超时直接放弃 EEPROM:MAC 走 RAL/RAH 备用读取路径).*/
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
	// Bit 26 是 Device Reset
	e1000_write(REG_CTRL, ctrl | (1 << 26));

	// 简单的延时，等待复位完成(也可以轮询该位自动清零)
	for (volatile int i = 0; i < 10000; i++) {
		/* spin */
	}
}

void e1000_init_rx(e1000_driver_t* nic)
{
	// 禁用中断
	e1000_write(REG_IMC, 0xFFFFFFFF);

	// 初始化环
	e1000_init_rx_ring(nic);

	// 明确设置 RCTL
	// 配置接收控制(最后开启 EN)
	// BAM: 接收广播包；SECRC: 去除 CRC 校验码；SZ_2048: 缓冲区 2KB
	// Bit 4 (MPE): 接收所有组播(防止某些广播被过滤)
	// Bit 15 (BAM): 广播
	// Bit 26 (SECRC): 剥离 CRC
	uint32_t rctl = RCTL_EN | RCTL_BAM | RCTL_MPE | RCTL_SZ_2048 | RCTL_SECRC;
	e1000_write(REG_RCTL, rctl);

	L_TAG(LOG_NET, "[E1000] RX Initialized. RCTL: %x, RAL/RAH: %x:%x\n",
			e1000_read(REG_RCTL), e1000_read(REG_RAL), e1000_read(REG_RAH));
}

/** 批量轮询接收描述符环。
 *
 * nic:网卡驱动实例。
 * out:接收到的包缓冲区数组。
 * max_batch:单次扫描最大包数量。
 *
 * 与单包版本的关键区别：
 *   - 不在循环内部写 REG_RDT，避免硬件在调用者读取数据之前
 *     就复用同一个缓冲区(这是原单包版本里隐藏的竞态条件).
 *   - 一次性扫描最多 max_batch 个描述符，减少函数调用和分支开销。
 *
 * 返回值：实际收集到的包数量(0 表示当前没有新包).
 */
int e1000_poll_packet_batch(e1000_driver_t *nic, e1000_rx_packet_t *out, int max_batch)
{
	int count = 0;
	uint32_t i = nic->rx_tail;

	while (count < max_batch) {
		struct e1000_rx_desc *desc = &nic->rx_ring[i];

#ifdef __aarch64__
		arch_dma_sync_for_cpu(desc, sizeof(*desc));
#endif
		/* 检查 DD (Descriptor Done) 位，没有新包就停止扫描 */
		if (!(desc->status & (1 << 0))) {
			break;
		}

		/* 收集这个包的信息，先不清状态，不写 RDT */
		out[count].data = nic->rx_buffers[i];
		out[count].len = desc->length;
		out[count].desc_idx = i;

		count++;
		i = (i + 1) % NUM_RX_DESC;
	}

	return count;
}

/** e1000_poll_packet_batch_swap - 单线程批量版，立即归还槛位
 *
 * nic:网卡驱动实例。
 * out:输出数组，存放扫描到的包。
 * max_batch:单次最大处理数量。
 *
 * 思路：和多线程流水线版一样做"换缓冲区"，
 * 但不经过队列/锁，扫描到的包直接放进调用者提供的数组，
 * 同一次循环里马上处理完，没有任何排队延迟。
 *
 * 效果：
 *   - 无数据竞争(处理的是换下来的旧缓冲区，硬件写的是全新缓冲区)
 *   - 无环容量收缩(每个描述符一旦被扫到，立刻换新槛位归还给硬件)
 *   - 无锁开销(单线程不需要同步)
 */
int e1000_poll_packet_batch_swap(e1000_driver_t *nic, e1000_rx_packet_t *out, int max_batch)
{
	int count = 0;
	uint32_t i = nic->rx_tail;
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

		/* 1. 把旧缓冲区(带数据)记录到输出数组 */
		out[count].data = nic->rx_buffers[i];
		out[count].len = desc->length;

		/* 2. 立即换上一个全新缓冲区，归还槛位给硬件
		 *    (单线程下直接 pmm_alloc，没有锁竞争，
		 *     也可以配合一个简单的无锁空闲池来减少分配开销) */
		uint64_t new_phys = (uint64_t)pmm_alloc();
		if (!new_phys) {
			s_stats.rx_dropped_poolempty++;
			break;
		}
		nic->rx_buffers[i] = phys_to_virt(new_phys);
		desc->buffer_addr = new_phys;
		desc->status = 0;

#ifdef __aarch64__
		/* Clean the replacement descriptor and buffer before DMA. */
		arch_dma_sync_for_device(desc, sizeof(*desc));
		arch_dma_sync_for_device(nic->rx_buffers[i], 2048);
#endif

		last_filled = i;
		any = 1;
		count++;
		i = (i + 1) % NUM_RX_DESC;
	}

	if (any) {
		nic->rx_tail = i;
		/* 批次里每个槛位换完缓冲区就立刻可用，
		 * 一次性写 RDT 把它们全部交还给硬件 */
		e1000_write(REG_RDT, last_filled);
	}

	return count;
}

/** e1000_rx_batch_release - 描述符归还环，不再持有包数据
 *
 * nic:网卡驱动实例。
 * batch:待释放的包缓冲区数组。
 * count:batch 中的包数量。
 *
 * 调用者处理完一批包之后调用：
 *   1. 清除这批描述符的状态位(归还给硬件复用)
 *   2. 更新软件侧 rx_tail
 *   3. 只写一次 REG_RDT，告知硬件新的空闲位置
 *
 * 调用顺序约束：先用完 batch 里的数据，再调用本函数。
 */
void e1000_rx_batch_release(e1000_driver_t *nic, e1000_rx_packet_t *batch, int count)
{
	if (count <= 0) {
		return;
	}

	for (int k = 0; k < count; k++) {
		uint32_t idx = batch[k].desc_idx;
		nic->rx_ring[idx].status = 0;   /* 清状态位，准备复用 */
#ifdef __aarch64__
		arch_dma_sync_for_device(&nic->rx_ring[idx],
			sizeof(struct e1000_rx_desc));
#endif
	}

	/* rx_tail 推进到批次末尾的下一个位置 */
	nic->rx_tail = (batch[count - 1].desc_idx + 1) % NUM_RX_DESC;

	/* 硬件认为 RDT 之前的位置全部空闲，可以接收新包写入。
	 * 这里只写一次寄存器，而不是每包写一次——频繁写 MMIO 是性能杀手。 */
	e1000_write(REG_RDT, (nic->rx_tail + NUM_RX_DESC - 1) % NUM_RX_DESC);
}

// 简化的发送函数
int e1000_send_packet(void *_nic, void *data, uint16_t len)
{
	e1000_driver_t *nic = (e1000_driver_t*)_nic;
	if (len > E1000_TX_BUFFER_SIZE) {
		L("[E1000] TX packet too large: %u bytes (max %u)\n",
				len, E1000_TX_BUFFER_SIZE);
		return -1;
	}

	/* A descriptor and its backing buffer remain exclusively owned until DD. */
	spin_lock(&nic->tx_lock);

	// 假设有 tx_tail 记录当前位置
	uint32_t i = nic->tx_tail;
	struct e1000_tx_desc *desc = &nic->tx_ring[i];

	// 1. 拷贝数据到发送缓冲区 (物理内存)
	memcpy(nic->tx_buffers[i], data, len);

	// [新增]获取该缓冲区的物理地址并填入描述符
	// 由于在 init_tx 中是用 pmm_alloc() 分配的，需要通过 vmm 转换回物理地址

	// 2. 填写描述符
	desc->length = len;
	desc->cmd = TX_CMD_EOP | TX_CMD_IFCS | TX_CMD_RS;
	desc->status = 0;

	// 3. 更新索引并告知硬件
	nic->tx_tail = (i + 1) % NUM_TX_DESC;
#ifdef __x86_64__
	__asm__ volatile("clflush (%0)" : : "r"(desc) : "memory");
#elif defined(__aarch64__)
	/* Clean descriptor and payload before the device reads them. */
	arch_dma_sync_for_device(nic->tx_buffers[i], len);
	arch_dma_sync_for_device(desc, sizeof(*desc));
#endif
	e1000_write(REG_TDT, nic->tx_tail);

	/* 埋点：入队即计数(不等 DD)，dd_timeout 单独计 */
	s_stats.tx_packets++;
	s_stats.tx_bytes += len;

	// 4. 等待发送完成 (DD位) : 实际开发中可用异步清理，这里简单起见轮询
	uint32_t iter = 0;
	while (!(desc->status & 0x01)) {
		if (++iter > 10000000) {
			L("[E1000] TX DD timeout TDH=%d TDT=%d status=%x\n",
					e1000_read(REG_TDH), e1000_read(REG_TDT), desc->status);
			s_stats.tx_dd_timeout++;
			spin_unlock(&nic->tx_lock);
			return -1;
		}
	}

	spin_unlock(&nic->tx_lock);
	return 0;
}

void e1000_init_tx(e1000_driver_t *nic)
{
	// 1. 分配 TX 描述符环内存 (同样需要物理连续且 16 字节对齐)
	void *tx_phys = pmm_alloc();
	if (!tx_phys) {
		L("[E1000] Failed to allocate TX descriptor ring\n");
		return;
	}
	nic->tx_ring = (struct e1000_tx_desc *)phys_to_virt((uint64_t)tx_phys);
	/* 同 rx_ring:HHDM 虚地址直接减偏移即物理地址 */
	nic->tx_ring_phys = (uint64_t)virt_to_phys((void*)nic->tx_ring);
	L("nic->tx_ring_phys %lx\n", nic->tx_ring_phys);
	memset(nic->tx_ring, 0, 4096);

	// 2. 为发送环分配固定缓冲区 (或者每次发送时动态映射，简单起见先分配)
	for (int i = 0; i < NUM_TX_DESC; i++) {
		uint64_t phys = (uint64_t)pmm_alloc(); // 假设返回物理地址
		if (!phys) {
			L("[E1000] Failed to allocate TX buffer %d\n", i);
			return;
		}
		nic->tx_buffers[i] = phys_to_virt(phys);
		nic->tx_ring[i].buffer_addr = phys; // 预先填入物理地址
		nic->tx_ring[i].status = E1000_TXD_STAT_DD    ; // 初始设为已完成
	}

	// 3. 配置硬件寄存器
	e1000_write(REG_TDBAL, (uint32_t)nic->tx_ring_phys);
	e1000_write(REG_TDBAH, (uint32_t)(nic->tx_ring_phys >> 32));
	e1000_write(REG_TDLEN, NUM_TX_DESC * 16);

	// 4. 初始化头尾指针
	e1000_write(REG_TDH, 0);
	e1000_write(REG_TDT, 0);
	nic->tx_tail = 0;
	spin_lock_init(&nic->tx_lock);
	nic->tx = e1000_send_packet;

	// 5. 开启发送控制 (TCTL)
	// EN: 开启， PSP: 填充短包， CT: 冲突检测， COLD: 全双工建议值 0x40
	uint32_t tctl = (1 << 1) | (1 << 3) | (0x0F << 4) | (0x40 << 12);
	e1000_write(REG_TCTL, tctl);

	// IEEE 802.3 标准建议值:IPGT=10, IPGR1=8, IPGR2=6
	// 写入 0x0060200A (即 6 << 20 | 8 << 10 | 10)
	e1000_write(0x0410, 0x0060200A);
}

/** e1000_read_mac - 读取网卡 MAC 地址
 *
 * mac:输出缓冲区，至少 6 字节。
 *
 * 原理：通过 EERD 寄存器轮询 EEPROM 中的数据。
 */
void e1000_read_mac(uint8_t* mac)
{
	// 优先尝试从 RAL/RAH 读取(网卡初始化后会自动从 EEPROM 加载到这里)
	uint32_t ral = e1000_read(0x5400);
	uint32_t rah = e1000_read(0x5404);

	// 如果 RAL/RAH 全为 0 或全为 F(说明未加载)，才尝试复杂的 EERD 读取
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
			// 写入读取请求：设置起始位 (bit 0) 和地址 (bit 8-23)
			// MAC 地址在 EEPROM 的前三个 Word (0, 1, 2)
			e1000_write(REG_EERD, 1 | (i << 8));

			// 轮询等待读取完成标志位 (bit 4: Done)，加 iter 上限
			uint32_t iter = 0;
			while (!((temp = e1000_read(REG_EERD)) & (1 << 4))) {
				if (++iter > 10000000) {
					L("[E1000] EERD timeout word=%d EERD=%x\n", i, temp);
					return;
				}
			}

			// 拿到底部 16 位数据
			mac[i * 2]     = (uint8_t)(temp >> 16);
			mac[i * 2 + 1] = (uint8_t)(temp >> 24);
		}
	}

	L_TAG(LOG_NET, "[E1000] MAC Address: %02x:%02x:%02x:%02x:%02x:%02x\n",
			mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void e1000_check_status()
{
	uint32_t status = e1000_read(REG_STATUS);
	L("[E1000] Status Register: %x\n", status);

	// Bit 1 为 Link Up 标志
	if (status & (1 << 1)) {
		L_TAG(LOG_NET, "[E1000] Link is UP. Speed: %s\n",
				(status & (1 << 6)) ? "1000Mbps" : "10/100Mbps");
	} else {
		L_TAG(LOG_NET, "[E1000] Link is DOWN.\n");
	}
}

e1000_driver_t *e1000_alloc()
{
	return kmalloc(sizeof(e1000_driver_t));
}

static void arp_cache_init(void);  /* fwd */

/* Global driver pointer for interrupt handler */
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
	// 打印第一个 BAR 地址(通常是 MMIO 寄存器基址)
	// BAR0 解析：网卡驱动的第一步就是读取 BAR0.
	// 它告诉内核网卡寄存器被映射到了哪个物理地址。
	/* Use dev->bar_phys[0] from enumeration. If 0 (no BIOS on ARM64),
	 * fall back to writing the BAR ourselves. */
	uint64_t bar0 = dev->bar_phys[0];
	if (!bar0) {
		/* QEMU virt PCI MMIO window: 0x10000000-0x3eff0000.
		 * e1000 BAR0 needs 128KB, use 0x10000000. */
		bar0 = 0x10000000;
		pci_write32(dev->bus, dev->slot, dev->func, PCI_REG_BAR0,
			    (uint32_t)(bar0 & ~0xF));
	}
	L_TAG(LOG_NET, "BAR0: 0x%llx (Memory Space)", bar0);

	e1000_map_bar(bar0);

	// 网卡作为 PCI 设备，必须在 PCI 配置空间开启 Bus Mastering,
	// 否则它没有权限发起 DMA 写入内存。
	uint16_t cmd = pci_read16(dev->bus, dev->slot, dev->func, 0x04);
	// 0x07 = IO + Memory + Bus Master
	pci_write16(dev->bus, dev->slot, dev->func, 0x04, cmd | 0x07);

	e1000_driver_t *drv = e1000_alloc();
	if (!drv) {
		panic("kmalloc(e1000_driver_t)");
	}

	arp_cache_init();
	e1000_reset();
	e1000_acquire_eeprom();
	e1000_read_mac(drv->mac);
	e1000_check_status();
	e1000_init_rx(drv);
	e1000_init_tx(drv);

	g_e1000_drv = drv;  /* register for ISR */

#ifndef __aarch64__
	/* x86_64 keeps this driver loadable, so IDT must call this instance. */
	idt_register_e1000_irq_handler(e1000_irq_handler);
#endif

	/* Keep RX descriptor consumption in the polling path for now.  This
	 * phase only proves PCI INTx delivery and records the interrupt cause. */
	(void)e1000_read(REG_ICR);

	int irq_ok = 0;
#ifdef __aarch64__
	/* ARM64 has an MSI dispatch path; use INTx when MSI is unavailable. */
	if (pci_enable_msi(dev) == 0) {
		e1000_write(REG_IMS,
			E1000_INT_RXT0 | E1000_INT_RXDMT0 | E1000_INT_RXO);
		L_TAG(LOG_NET, "[E1000] MSI enabled: IMS=%x\n",
			e1000_read(REG_IMS));
		irq_ok = 1;
	}
#endif
	if (!irq_ok && pci_enable_intx(dev) == 0) {
		e1000_write(REG_IMS,
			E1000_INT_RXT0 | E1000_INT_RXDMT0 | E1000_INT_RXO);
		L_TAG(LOG_NET, "[E1000] INTx enabled: IRQ %u IMS=%x\n",
			dev->irq_line, e1000_read(REG_IMS));
		irq_ok = 1;
	} else {
		if (!irq_ok)
			L_TAG(LOG_NET,
				"[E1000] no IRQ route available; polling only\n");
	}

	return drv;
}

/** e1000_exit - 网卡清理：停 RX、写 RCTL 禁接收、注销埋点。
 *
 * drv: 驱动实例指针（e1000_init 返回值）。 */
void e1000_exit(e1000_driver_t *drv)
{
	if (!drv)
		return;

	/* 1. 禁用接收：清 RCTL.EN */
	uint32_t rctl = e1000_read(REG_RCTL);
	e1000_write(REG_RCTL, rctl & ~RCTL_EN);

	/* 2. 屏蔽所有中断 */
	e1000_write(REG_IMC, 0xFFFFFFFF);

#ifndef __aarch64__
	idt_register_e1000_irq_handler(NULL);
#endif

	/* 3. 注销埋点（API 约定：传 NULL 即卸载） */
	stats_net_register(NULL);

	L_TAG(LOG_NET, "[E1000] exit: RCTL=%x\n", e1000_read(REG_RCTL));
}

void e1000_handle_packet(e1000_driver_t *nic, uint8_t *packet, uint16_t len);  /* fwd */

/* ============================================================
 * ARP 缓存：简单固定大小表，表项数 = ARP_CACHE_SIZE
 * ============================================================ */
#define ARP_CACHE_SIZE 4

static struct {
	uint32_t ip;       /* 网络字节序 (LE uint32 on wire) */
	uint8_t  mac[6];
	int      valid;
} g_arp_cache[ARP_CACHE_SIZE];

static void arp_cache_init(void)
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++)
		g_arp_cache[i].valid = 0;
}

static void arp_cache_update(uint32_t ip, const uint8_t *mac)
{
	int slot = -1;
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip) {
			slot = i;  /* 更新已有条目 */
			break;
		}
		if (!g_arp_cache[i].valid && slot < 0)
			slot = i;  /* 首个空闲槽 */
	}
	if (slot < 0)
		slot = 0;  /* 表满→覆盖最旧 */
	g_arp_cache[slot].ip = ip;
	memcpy(g_arp_cache[slot].mac, mac, 6);
	g_arp_cache[slot].valid = 1;
}

static const uint8_t *arp_cache_lookup(uint32_t ip)
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++)
		if (g_arp_cache[i].valid && g_arp_cache[i].ip == ip)
			return g_arp_cache[i].mac;
	return NULL;
}

void e1000_handle_packet(e1000_driver_t *nic, uint8_t *packet, uint16_t len)
{
#ifdef __aarch64__
	/* Invalidate device-written payload before parsing it. */
	arch_dma_sync_for_cpu(packet, len);
#endif

	if (len < sizeof(struct eth_header)) {
		return;
	}

	struct eth_header *eth = (struct eth_header *)packet;
	uint16_t type = ntoh16(eth->type);

	/* ---- ARP 处理 ---- */
	if (type == 0x0806) {
		s_stats.arp_rx++;
		struct arp_packet *arp = (struct arp_packet *)(packet + sizeof(struct eth_header));
		uint16_t opcode = ntoh16(arp->opcode);

		/* case 1: ARP 请求 → 如果是问 192.168.56.0/24 子网内的 IP，回复 */
		/* subnet 192.168.56.0/24 = 0xXX38A8C0 (LE uint32 from network order) */
		if (opcode == 1 && (arp->dest_ip & 0x00FFFFFF) == 0x0038A8C0) {
			send_arp_reply(nic, eth, arp);
			s_stats.arp_replies++;
		}
		/* case 2: ARP 回复 → 缓存 sender 的 MAC-IP 映射 */
		else if (opcode == 2) {
			/* 只缓存与我们网段相关的条目 */
			arp_cache_update(arp->src_ip, arp->src_mac);
			L_TAG(LOG_NET, "[ARP] reply from %d.%d.%d.%d → "
				"%02x:%02x:%02x:%02x:%02x:%02x\n",
				(int)(arp->src_ip & 0xFF),
				(int)((arp->src_ip >> 8) & 0xFF),
				(int)((arp->src_ip >> 16) & 0xFF),
				(int)((arp->src_ip >> 24) & 0xFF),
				arp->src_mac[0], arp->src_mac[1], arp->src_mac[2],
				arp->src_mac[3], arp->src_mac[4], arp->src_mac[5]);
		}
	} else if (0x0800 == type) {
		struct ip_header *ip = (struct ip_header *)(packet + sizeof(struct eth_header));
		if (ip->proto == 1) { // ICMP
			struct icmp_header *icmp = (struct icmp_header *)(packet + sizeof(struct eth_header) + sizeof(struct ip_header));

			if (icmp->type == 8) { // Echo Request
				s_stats.icmp_echo_rx++;
				int ip_total_len = ntoh16(ip->len);
				int payload_len = ip_total_len - sizeof(struct ip_header) - sizeof(struct icmp_header);
				send_icmp_reply(nic, eth, ip, icmp, payload_len);
				s_stats.icmp_echo_reply++;
			} else if (icmp->type == 0) { // Echo Reply
				L_TAG(LOG_NET, "[ICMP] Echo Reply from %d.%d.%d.%d "
					"seq=%u id=0x%x len=%u\n",
					(int)(ip->src_ip & 0xFF),
					(int)((ip->src_ip >> 8) & 0xFF),
					(int)((ip->src_ip >> 16) & 0xFF),
					(int)((ip->src_ip >> 24) & 0xFF),
					ntoh16(icmp->seq), ntoh16(icmp->id),
					ntoh16(ip->len));
			}
		} else {
			s_stats.rx_dropped_unknown_proto++;
		}
	} else {
		s_stats.rx_dropped_unknown_ether++;
	}
}

void e1000_send_test_broadcast(e1000_driver_t *nic)
{
	uint8_t dummy[64];
	memset(dummy, 0xff, 6); // DST: FF:FF:FF:FF:FF:FF
							// memcpy(dummy + 6, nic->mac, 6); // SRC
	memset(dummy + 6, 0xcc, 6); // SRC
	dummy[12] = 0x08; dummy[13] = 0x00; // Type: IPv4 (随便填一个)
	memset(dummy + 14, 0xAB, 50); // Payload
	e1000_send_packet(nic, dummy, 64);
}

void *e1000_poll_packet(e1000_driver_t *nic, uint16_t *out_len)
{
	uint32_t i = nic->rx_tail;
	struct e1000_rx_desc *desc = &nic->rx_ring[i];

#ifdef __aarch64__
	arch_dma_sync_for_cpu(desc, sizeof(*desc));
#endif

	// 1. 强制读取内存，防止编译器优化导致的读取旧值
	// 如果在开启了缓存的区域，这里可能需要 clflush (如果没设置 PCD/PWT)
	// __asm__ volatile("clflush (%0)" : : "r"(desc) : "memory");
	// 检查 DD 位
	if (!(desc->status & (1 << 0))) {
		return NULL;
	}

	t0 = rdtsc();
	// 2. 插入指令屏障，确保在处理数据前，描述符的所有位都已经从内存加载
	// 在 e1000_map_bar 时已经为描述符所在的内存区域设置了 PTE_CACHE_DISABLE (PCD),
	// 那么 CPU 根本不会缓存这段内存，clflush 此时是多余的，反而会浪费几百个 cycles.
	// __asm__ volatile ("lfence" ::: "memory");

	// 3. 处理业务逻辑 (比如交给协议栈)
	*out_len = desc->length;
	void* buffer_virt = nic->rx_buffers[i];

#ifdef __aarch64__
	arch_dma_sync_for_cpu(buffer_virt, *out_len);
#endif

	// 4. 清理状态位，准备还给硬件
	// 清除状态位供下次使用
	desc->status = 0;

	// 5. 更新索引
	nic->rx_tail = (i + 1) % NUM_RX_DESC;

	// 6. 更新硬件 Tail。批量优化见 e1000_poll_and_enqueue。
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
		void* packet = e1000_poll_packet(nic, &len);
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
				e1000_handle_packet(nic, (uint8_t*)batch[k].data, batch[k].len);
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
				e1000_handle_packet(nic, (uint8_t*)batch[k].data, batch[k].len);
			}

			t3 = rdtsc();
			// L("RX batch: %d packets, PROC: %lu", n, t3 - t2);

		} else {
			schedule();
		}
	}
}

typedef struct {
	uint64_t   phys[RX_FREE_POOL_SIZE];
	uint32_t   count;
	spinlock_t lock;
} e1000_free_pool_t;

static e1000_free_pool_t g_rx_free_pool;

/* 预先分配 target_count 个物理页放进池子
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
		if (!page)
			break;
		pool->phys[pool->count++] = page;
	}
	spin_unlock(&pool->lock);
}

/* 取一个空闲物理页；池子空了就退化为直接分配(慢路径，理论上不该频繁触发) */
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
		s_stats.rx_dropped_poolempty++;
		phys = (uint64_t)pmm_alloc();
	}

	return phys;
}

/* 处理完报文后，把缓冲区还回池子；池子满了就直接释放给物理内存 */
static void rx_free_pool_put(e1000_free_pool_t *pool, uint64_t phys)
{
	spin_lock(&pool->lock);

	if (pool->count < RX_FREE_POOL_SIZE) {
		pool->phys[pool->count++] = phys;
		spin_unlock(&pool->lock);
		return;
	}

	spin_unlock(&pool->lock);

	pmm_free((void*)phys);
}

typedef struct {
	e1000_rx_entry_t entries[RX_QUEUE_SIZE];
	/* 消费者(处理线程)从这里取 */
	uint32_t   head;
	/* 生产者(轮询函数)往这里放 */
	uint32_t   tail;
	uint32_t   count;
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
		if (q->count > s_stats.rx_queue_hwm) {
			s_stats.rx_queue_hwm = q->count;
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

/** e1000_poll_and_enqueue - 扫描收到的报文，立即换上新缓冲区
 *
 * nic:网卡驱动实例。
 *
 * 对每个就绪描述符：
 *   1. 从空闲池取一个全新的物理页
 *   2. 把这个新页绑定到描述符 i(buffer_addr 和 rx_buffers[i] 都更新)
 *      : 这一步之后，硬件下次往这个槛位写数据时，
 *         写的是全新的内存，绝不会碰到还没处理完的旧数据
 *   3. 把原来装着数据的缓冲区(连同长度，物理地址)推入处理队列
 *   4. 清状态位 + 更新 RDT，硬件立刻可以继续往这里收包
 *
 * 处理逻辑完全解耦：处理线程从队列里慢慢取，
 * 用完后调用 e1000_rx_release_buffer() 归还缓冲区。
 */
int e1000_poll_and_enqueue(e1000_driver_t *nic)
{
	int n = 0;
	uint32_t i = nic->rx_tail;
	uint32_t last_filled = i;

	while (1) {
		struct e1000_rx_desc *desc = &nic->rx_ring[i];

#ifdef __aarch64__
		arch_dma_sync_for_cpu(desc, sizeof(*desc));
#endif
		if (!(desc->status & (1 << 0))) {
			break;   /* 没有更多就绪的包 */
		}

		/* 1. 准备一个干净的新缓冲区 */
		uint64_t new_phys = rx_free_pool_get(&g_rx_free_pool);
		void *new_virt = phys_to_virt(new_phys);

		/* 2. 把旧缓冲区(装着报文数据)打包推入处理队列 */
		e1000_rx_entry_t entry;
		entry.data = nic->rx_buffers[i];
		entry.len = desc->length;
		entry.buf_phys = desc->buffer_addr;

		if (!rx_queue_push(&g_rx_queue, &entry)) {
			/* 处理队列满了，说明消费者跟不上。
			 * 把刚取的新缓冲区还回池子，停止扫描：
			 * 这个描述符的 DD 位仍然是 1，数据没丢，
			 * 下次处理队列有空位时再来取，是天然的背压。 */
			rx_free_pool_put(&g_rx_free_pool, new_phys);
			s_stats.rx_dropped_qfull++;
			L("rx queue full, pausing at desc %u", i);
			break;
		}

		/* 3. 把新缓冲区绑定到这个描述符 */
		nic->rx_buffers[i] = new_virt;
		desc->buffer_addr = new_phys;
		desc->status = 0;        /* 归还描述符给硬件 */

#ifdef __aarch64__
		arch_dma_sync_for_device(desc, sizeof(*desc));
		arch_dma_sync_for_device(new_virt, 2048);
#endif

		/* 埋点：成功入 rx_queue */
		s_stats.rx_packets++;
		s_stats.rx_bytes += entry.len;

		last_filled = i;
		n++;
		i = (i + 1) % NUM_RX_DESC;
	}

	if (n > 0) {
		nic->rx_tail = i;
		/* 只写一次 RDT，告诉硬件新空出来的描述符范围 */
		e1000_write(REG_RDT, last_filled);
	}

	return n;
}

int e1000_rx_dequeue_packet(e1000_rx_entry_t *out)
{
	return rx_queue_pop(&g_rx_queue, out);
}

/* 处理完报文数据后必须调用，否则空闲池会逐渐枯竭 */
void e1000_rx_release_buffer(e1000_rx_entry_t *entry)
{
	rx_free_pool_put(&g_rx_free_pool, entry->buf_phys);
}

void e1000_rx_pipeline_init(void)
{
	/* 幂等保护:memset 会清掉 spinlock state 和已 push 的 entry/phys，
	 * 二次进入相当于撕掉一切重来：如果有线程正持锁就直接破坏一致性。
	 * 这里用 static 标志拦住后续调用，多个 test_loop_* 函数都能放心 init.
	 *
	 * 注：依赖 module BSS 独立分配 + 清零 (见 kernel/thread.c
	 * SHT_NOBITS 处理).早期版本 module BSS 与 .rodata 共享文件偏移，
	 * inited 读到 rodata 字节大概率非零 -> 直接 return 跳过 memset/refill
	 * -> 100% CPU 热循环。已在 kernel 侧修复。*/
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
		/* 尽快把网卡收到的包搬到队列里，硬件能马上继续收 */
		int n = e1000_poll_and_enqueue(nic);

		/* 慢慢处理队列里的包，处理速度不影响硬件收包 */
		int processed = 0;
		while (processed < RX_BATCH_MAX && e1000_rx_dequeue_packet(&entry)) {
			e1000_handle_packet(nic, (uint8_t*)entry.data, entry.len);
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

	nic = (e1000_driver_t*)data;
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

	nic = (e1000_driver_t*)data;
	e1000_rx_entry_t entry;
	uint64_t hw_last_tick = atomic64_read(&timer_ticks);

	while (1) {
		/* 批量处理 RX_BATCH_MAX 包再让出，与 _test_loop_st 行为对齐，
		 * 减少单包一对 lock acquire/release 的开销 */
		int processed = 0;
		while (processed < RX_BATCH_MAX && e1000_rx_dequeue_packet(&entry)) {
			e1000_handle_packet(nic, (uint8_t*)entry.data, entry.len);
			e1000_rx_release_buffer(&entry);
			processed++;
		}

		if (processed > 0) {
			s_stats.rx_processed += processed;
			/* 每批处理完刷一次衍生字段(ring head/tail,queue/pool count,link)*/
			e1000_stats_snapshot(nic);
		}

		/* 硬件计数节流刷新:TIMER_HZ=100 -> 50 tick 就是 500ms.
		 * 无论 processed 是否为 0 都要检查：链路空闲时 hw counter 也可能
		 * 因 RX_NO_BUFFERS/CRC/collision 等异常在累积，不刷会溢出丢失。 */
		if (atomic64_read(&timer_ticks) - hw_last_tick >= TIMER_HZ / 2) {
			e1000_hw_stats_refresh();
			hw_last_tick = atomic64_read(&timer_ticks);
		}

		if (processed == 0) {
			e1000_idle();
		}
	}
}

/*
 * 展示前刷新一次"衍生"字段：从 driver / queue / pool 读快照。
 * 这些字段不是"计数"，是实时状态镜像，不能靠埋点累加，只能读一次填一次。
 * 挂在 rx_process_thread 每 batch 更新一次即可(1Hz monitor 撞不撞得上无所谓，
 * 收敛到 100ms 内的旧值不影响展示).
 */
static void e1000_stats_snapshot(e1000_driver_t *nic)
{
	if (!nic) {
		return;
	}
	s_stats.rx_ring_tail = nic->rx_tail;
	s_stats.tx_ring_tail = nic->tx_tail;
	s_stats.rx_ring_head = e1000_read(REG_RDH);
	s_stats.tx_ring_head = e1000_read(REG_TDH);
	s_stats.rx_queue_count = g_rx_queue.count;
	s_stats.free_pool_count = g_rx_free_pool.count;

	uint32_t status = e1000_read(REG_STATUS);
	s_stats.link_up = (status & (1u << 1)) ? 1 : 0;
	s_stats.link_speed = (status & (1u << 6)) ? 1000 : 100;
	memcpy(s_stats.mac, nic->mac, 6);
	s_stats.rx_mode = g_rx_mode;
	s_stats.idle_mode = g_idle_mode;
}

/*
 * 硬件 MMIO 计数刷新 : 读出 delta 累加到 s_stats.hw_*.
 * 调用点：仅 rx_process_thread 一处，节流到 ~500ms/次。
 *   1. "读即清"寄存器不能被多处并发读，否则计数丢失
 *   2. 32bit 计数在极端速率下有溢出周期(GORC 1Gbps ~= 34s),
 *      500ms 采样窗口对最激进的字节计数也有 68x 余量
 *
 * GORCL/H,GOTCL/H:手册规定先读 L 触发 H latch，再读 H 拿到 latched 值
 * 并触发整对清零。顺序反了 -> 拿到脏 H.
 */
static void e1000_hw_stats_refresh(void)
{
	s_stats.hw_crcerrs += e1000_read(REG_CRCERRS);
	s_stats.hw_mpc     += e1000_read(REG_MPC);
	s_stats.hw_colc    += e1000_read(REG_COLC);
	s_stats.hw_gprc    += e1000_read(REG_GPRC);
	s_stats.hw_gptc    += e1000_read(REG_GPTC);

	/* 64bit 字节计数:L 必须先读 */
	uint32_t gorcl = e1000_read(REG_GORCL);
	uint32_t gorch = e1000_read(REG_GORCH);
	s_stats.hw_gorc += ((uint64_t)gorch << 32) | gorcl;

	uint32_t gotcl = e1000_read(REG_GOTCL);
	uint32_t gotch = e1000_read(REG_GOTCH);
	s_stats.hw_gotc += ((uint64_t)gotch << 32) | gotcl;

	s_stats.hw_rnbc += e1000_read(REG_RNBC);
	s_stats.hw_tpr  += e1000_read(REG_TPR);
	s_stats.hw_tpt  += e1000_read(REG_TPT);
}

void e1000_test_loop_mt(void *data)
{
	e1000_rx_pipeline_init();

	int cpu = cpu_get_ctx()->id;
	struct thread *t = thread_create_on(e1000_rx_process_thread, data, cpu < 0 ? 0 : cpu);
	thread_set_name(t, "e1000_proc");
	cpu_enqueue(-1, t);
	// thread_set_name(get_current(), "e1000_rx");
	e1000_rx_poll_thread((e1000_driver_t*)data);
}

/** e1000_probe_gateway - 发送 ARP 请求探测网关 MAC
 *
 * nic:网卡驱动实例。
 * gateway_ip:目标 IP（网络字节序，如 10.0.2.2 = 0x0A000202）。
 *
 * QEMU user-mode 网关固定为 10.0.2.2，发 ARP request 测 TX+RX 端到端通路。
 */
void e1000_probe_gateway(e1000_driver_t *nic, uint32_t gateway_ip)
{
	uint8_t frame[64];
	memset(frame, 0, sizeof(frame));

	/* Ethernet: broadcast dst, our MAC src, ARP type */
	memset(frame, 0xFF, 6);
	memcpy(frame + 6, nic->mac, 6);
	frame[12] = 0x08; frame[13] = 0x06;

	/* ARP 请求:偏移 14 */
	struct arp_packet *arp = (struct arp_packet *)(frame + sizeof(struct eth_header));
	arp->hw_type   = hton16(1);
	arp->proto_type = hton16(0x0800);
	arp->hw_addr_len = 6;
	arp->proto_addr_len = 4;
	arp->opcode    = hton16(1);  /* Request */
	memcpy(arp->src_mac, nic->mac, 6);
	arp->src_ip    = hton32(0xC0A8380F);  /* 192.168.56.15 */
	memset(arp->dest_mac, 0, 6);
	arp->dest_ip   = hton32(gateway_ip);

	e1000_send_packet(nic, frame, sizeof(struct eth_header) + sizeof(struct arp_packet));
	L_TAG(LOG_NET, "[E1000] ARP probe sent to %d.%d.%d.%d\n",
		(int)((gateway_ip >> 24) & 0xFF), (int)((gateway_ip >> 16) & 0xFF),
		(int)((gateway_ip >> 8) & 0xFF), (int)(gateway_ip & 0xFF));
}

/** e1000_ping_gateway - 发 ICMP Echo Request 到网关
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

	/* 固定 payload: "LaOS ping!"，12 字节 */
	static const char *payload = "LaOS ping!";
	int pay_len = 12;

	uint8_t frame[256];
	memset(frame, 0, sizeof(frame));

	/* --- Ethernet --- */
	struct eth_header *eth = (struct eth_header *)frame;
	memcpy(eth->dest, gw_mac, 6);
	memcpy(eth->src, nic->mac, 6);
	eth->type = hton16(0x0800);

	/* --- IP --- */
	struct ip_header *ip = (struct ip_header *)(frame + sizeof(struct eth_header));
	ip->version_ihl = 0x45;  /* version 4, IHL 5 */
	ip->tos = 0;
	ip->len = hton16(sizeof(struct ip_header) + sizeof(struct icmp_header) + pay_len);
	ip->id = hton16(0x0001);
	ip->flags_offset = 0;
	ip->ttl = 64;
	ip->proto = 1;  /* ICMP */
	ip->src_ip = hton32(0xC0A8380F);  /* 192.168.56.15 */
	ip->dest_ip = hton32(gateway_ip);
	ip->checksum = 0;
	ip->checksum = net_checksum(ip, sizeof(struct ip_header));

	/* --- ICMP --- */
	struct icmp_header *icmp = (struct icmp_header *)(frame + sizeof(struct eth_header) + sizeof(struct ip_header));
	icmp->type = 8;  /* Echo Request */
	icmp->code = 0;
	icmp->id = hton16(0x1234);
	icmp->seq = hton16(1);
	memcpy((uint8_t *)icmp + sizeof(struct icmp_header), payload, pay_len);
	icmp->checksum = 0;
	icmp->checksum = net_checksum(icmp, sizeof(struct icmp_header) + pay_len);

	int total_len = sizeof(struct eth_header) + sizeof(struct ip_header)
	              + sizeof(struct icmp_header) + pay_len;
	int ret = e1000_send_packet(nic, frame, total_len);
	if (ret == 0)
		L_TAG(LOG_NET, "[ICMP] Echo Request sent to %d.%d.%d.%d\n",
			(int)((gateway_ip >> 24) & 0xFF), (int)((gateway_ip >> 16) & 0xFF),
			(int)((gateway_ip >> 8) & 0xFF), (int)(gateway_ip & 0xFF));
	return ret;
}

/* 模块入口: main(argc, argv)
 *
 * MODULE_PARAM 声明的变量（如 g_idle_mode）由内核在加载后、本函数执行前
 * 根据 task.conf 的 key=value 对自动写入，无需手动解析。
 *
 * 返回时由 thread_entry_point 调用 thread_exit(code)。 */
int main(int argc, char *argv[])
{
	e1000_driver_t *drv;

	(void)argc;
	(void)argv;
	/* g_idle_mode 已由内核根据 task.conf 的 key=value 参数预先写入 */

	drv = e1000_init(NULL);

	if (drv) {
		/* 把埋点结构地址交给 kernel/stats.c，供 stats_net() 展示 */
		stats_net_register(&s_stats);
		e1000_stats_snapshot(drv);

		/* 收包模式由 task.conf 中 g_rx_mode 选择，默认 5 (MT) */
		switch (g_rx_mode) {
		case 1:  e1000_test_loop(drv);          break;
		case 2:  e1000_test_loop_batch(drv);    break;
		case 3:  e1000_test_loop_batch_swap(drv); break;
		case 4:  e1000_test_loop_st(drv);       break;
		default: e1000_test_loop_mt(drv);       break;
		}

		e1000_exit(drv);
	}

	L("E1000 thread exiting");
	return 0;
}

/* Minimal ISR: acknowledge the device, record the cause and wake the RX
 * worker.  RX ring ownership remains exclusively in thread context. */
void e1000_irq_handler(void)
{
	if (!g_e1000_drv)
		return;
	uint32_t cause = e1000_read(REG_ICR);
	if (!cause)
		return;
	g_e1000_last_icr = cause;
	uint64_t count = ++g_e1000_irq_count;
	if (cause & (E1000_INT_RXT0 | E1000_INT_RXDMT0 | E1000_INT_RXO))
		__atomic_store_n(&g_e1000_rx_pending, 1, __ATOMIC_RELEASE);

	struct thread *worker = __atomic_load_n(&g_e1000_rx_worker,
		__ATOMIC_ACQUIRE);
	if (worker && thread_get_status(worker) == THREAD_BLOCKED) {
		thread_set_status(worker, THREAD_READY);
		struct cpu_context *target = g_cpu_contexts[worker->target_cpu];
		if (target)
			target->need_resched = 1;
	}
	if (count <= 8)
		kprintf("[e1000] IRQ #%llu ICR=0x%x\n", count, cause);
}

void e1000_wait_rx_event(void)
{
	struct thread *current = get_current();
	__atomic_store_n(&g_e1000_rx_worker, current, __ATOMIC_RELEASE);

	for (;;) {
		uint64_t irq_flags = save_and_disable_interrupts();
		if (__atomic_exchange_n(&g_e1000_rx_pending, 0,
				__ATOMIC_ACQ_REL)) {
			restore_interrupts(irq_flags);
			return;
		}

		/* IRQ is masked across the final check and BLOCKED transition, so
		 * the ISR cannot post an event between them.  After restore, either
		 * the ISR wakes us before schedule(), or schedule() blocks us. */
		thread_set_status(current, THREAD_BLOCKED);
		current->sleep_times++;
		restore_interrupts(irq_flags);
		schedule();
	}
}

#ifndef __STATE_H__
#define __STATE_H__

/*
 * stats.h - 统计指标结构定义
 */

#include <stdint.h>

void stats_cpu(void);
void stats_rcu(void);
void stats_net(void);
void stats_conf(void);
void stats_sys(void);

/*
 * e1000 stats : 结构定义由 kernel 提供，module 侧填埋点。
 *
 * 分层约定:kernel 定义 layout(stats.h)，module 定义一个 struct e1000_stats
 * 实例并埋计数点，加载时调 stats_net_register(&s_stats) 把地址交给 kernel.
 * stats.c 的 stats_net() 拿到指针后直接读，读侧不加锁：单纯的展示，
 * 计数是 uint64_t 加法，撕裂读到中间态无关正确性。
 *
 * 字段口径：所有计数都是"累计值"，从 module 加载起算，never reset.
 * stats.c 打印时按需算 delta 送 UI.
 */
struct e1000_stats {
	/* RX 软件路径： e1000_poll_and_enqueue / e1000_rx_process_thread 埋点 */
	uint64_t rx_packets; /* 从硬件描述符成功取到并入队的包 */
	uint64_t rx_bytes; /* 同上，累计字节 */
	uint64_t rx_dropped_qfull; /* rx_queue 满，本次 poll 停摆 */
	uint64_t rx_dropped_poolempty; /* free_pool 空 -> pmm_alloc 慢路径命中次数 */
	uint64_t rx_processed; /* process_thread 出队处理的包数 */

	/* TX 软件路径： e1000_send_packet 埋点 */
	uint64_t tx_packets; /* 入描述符环的包(不管 DD 是否超时) */
	uint64_t tx_bytes;
	uint64_t tx_dd_timeout; /* DD 位轮询超过 10M 次未完成 */

	/* L2/L3 breakdown: e1000_handle_packet + protocol.c 埋点 */
	uint64_t arp_rx; /* 收到的 ARP 包 */
	uint64_t arp_replies; /* 主动回复的 ARP */
	uint64_t icmp_echo_rx; /* 收到的 ICMP Echo Request */
	uint64_t icmp_echo_reply; /* 主动回复的 ICMP Echo */

	/* 未识别 */
	uint64_t rx_dropped_unknown_ether; /* eth type 非 ARP/IPv4 */
	uint64_t rx_dropped_unknown_proto; /* IPv4 但 proto 非 ICMP */

	/*
	 * 硬件 MMIO 计数(Intel 82540EM S13.4): 由 module 侧
	 * e1000_hw_stats_refresh() 每 ~500ms 刷一次，"读即清"寄存器
	 * 读出的 delta 累加到这里，得到"上电以来累计"值。
	 * 与上面软件计数的关系：
	 *   - hw_gprc ~= rx_packets(前者是硬件视角的"好包"，后者是软件成功入队；
	 *     两者差 = 因 rx_queue_qfull 停摆丢的包)
	 *   - hw_tpt ~= tx_packets
	 *   - hw_crcerrs / hw_mpc / hw_rnbc / hw_colc 是软件永远看不到的硬件视角
	 */
	uint64_t hw_crcerrs; /* CRC 错误包 */
	uint64_t hw_mpc; /* RX FIFO 满丢包 */
	uint64_t hw_gprc; /* Good Packets Received */
	uint64_t hw_gptc; /* Good Packets Transmitted */
	uint64_t hw_gorc; /* Good Octets Received (L+H) */
	uint64_t hw_gotc; /* Good Octets Transmitted (L+H) */
	uint64_t hw_tpr; /* Total Packets Received(含错) */
	uint64_t hw_tpt; /* Total Packets Transmitted */
	uint64_t hw_colc; /* Collision count */
	uint64_t hw_rnbc; /* RX No Buffers */

	/* 衍生指标： 展示时快照，不是"计数"，driver 层直接读 nic/queue 状态 */
	uint32_t rx_ring_head;
	uint32_t rx_ring_tail;
	uint32_t tx_ring_head;
	uint32_t tx_ring_tail;
	uint32_t rx_queue_count; /* g_rx_queue.count 快照 */
	uint32_t rx_queue_hwm; /* g_rx_queue.count 历史高水位 */
	uint32_t free_pool_count; /* g_rx_free_pool.count 快照 */
	uint32_t link_up; /* nic->link_status 快照，0/1 */
	uint32_t link_speed; /* Mbps */
	uint8_t  mac[6];
	uint8_t  _pad[2];

	/* 模块配置参数快照 (由 e1000_stats_snapshot 每 tick 刷新) */
	int32_t  rx_mode;    /* 1=simple 2=batch 3=batch_swap 4=single_thr 5=mt */
	int32_t  idle_mode;  /* 0=schedule(spin) 1=schedule_timeout(sleep) */
	int32_t  _pad2;      /* align to 8 bytes */
};

/*
 * module 加载完成后调用一次，把埋点结构地址交给 kernel.
 * 传 NULL 等价于卸载(当前 LaOS 无 module unload，保留语义完整性).
 * 单 writer(加载线程)单 reader(monitor 线程)，指针赋值天然原子，未加锁。
 */
void stats_net_register(const struct e1000_stats *p);

#endif

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
 * e1000 统计结构由内核定义，驱动模块负责更新埋点。
 *
 * 内核定义结构布局，模块持有实例并在加载时通过
 * stats_net_register(&s_stats) 注册。统计字段以 relaxed 原子操作读写，避免多写者
 * 更新丢失及与 monitor 并发访问产生的数据竞争；一次 monitor 刷新仍是逐字段读取，
 * 可能组合不同时刻的值。这些值仅用于诊断展示，不得用于同步判断或精确统计。
 *
 * 累计计数从模块加载时开始，运行期间不主动清零；stats.c 根据相邻快照计算
 * 差值并显示。
 */
struct e1000_stats {
	/* RX 软件路径： e1000_poll_and_enqueue / e1000_rx_process_thread 埋点 */
	uint64_t rx_packets;	       /* 从硬件描述符成功取到并入队的包 */
	uint64_t rx_bytes;	       /* 同上，累计字节 */
	uint64_t rx_dropped_qfull;     /* rx_queue 满，本次 poll 停摆 */
	uint64_t rx_dropped_poolempty; /* free_pool 空 -> pmm_alloc 慢路径命中次数 */
	uint64_t rx_processed;	       /* process_thread 出队处理的包数 */

	/* TX 软件路径： e1000_send_packet 埋点 */
	uint64_t tx_packets; /* 入描述符环的包(不管 DD 是否超时) */
	uint64_t tx_bytes;
	uint64_t tx_dd_timeout; /* DD 位轮询超过 10M 次未完成 */

	/* L2/L3 分类：由 e1000_handle_packet() 和 protocol.c 更新。 */
	uint64_t arp_rx;	  /* 收到的 ARP 包 */
	uint64_t arp_replies;	  /* 主动回复的 ARP */
	uint64_t icmp_echo_rx;	  /* 收到的 ICMP Echo Request */
	uint64_t icmp_echo_reply; /* 主动回复的 ICMP Echo */

	/* 不支持的以太网类型或 IPv4 协议。 */
	uint64_t rx_dropped_unknown_ether; /* eth type 非 ARP/IPv4 */
	uint64_t rx_dropped_unknown_proto; /* IPv4 但 proto 非 ICMP */

	/*
	 * 硬件 MMIO 计数(Intel 82540EM S13.4): 由 module 侧
	 * e1000_hw_stats_refresh() 每 ~500ms 刷一次，"读即清"寄存器
	 * 读出的 delta 累加到这里，得到"上电以来累计"值。
	 * 与上面软件计数的关系：
	 *   - hw_gprc 与 rx_packets 口径不同：前者是硬件收到的好包数，后者是软件成功
	 *     入队数；差值还可能包含轮询间隙、队列满等原因造成的软件未处理包。
	 *   - hw_tpt ~= tx_packets
	 *   - hw_crcerrs / hw_mpc / hw_rnbc / hw_colc 是软件永远看不到的硬件视角
	 */
	uint64_t hw_crcerrs; /* CRC 错误包 */
	uint64_t hw_mpc;     /* RX FIFO 满丢包 */
	uint64_t hw_gprc;    /* Good Packets Received */
	uint64_t hw_gptc;    /* Good Packets Transmitted */
	uint64_t hw_gorc;    /* Good Octets Received (L+H) */
	uint64_t hw_gotc;    /* Good Octets Transmitted (L+H) */
	uint64_t hw_tpr;     /* Total Packets Received(含错) */
	uint64_t hw_tpt;     /* Total Packets Transmitted */
	uint64_t hw_colc;    /* Collision count */
	uint64_t hw_rnbc;    /* RX No Buffers */

	/* 展示用快照，由驱动直接读取网卡或队列状态。 */
	uint32_t rx_ring_head;
	uint32_t rx_ring_tail;
	uint32_t tx_ring_head;
	uint32_t tx_ring_tail;
	uint32_t rx_queue_count;  /* g_rx_queue.count 快照 */
	uint32_t rx_queue_hwm;	  /* g_rx_queue.count 历史高水位 */
	uint32_t free_pool_count; /* g_rx_free_pool.count 快照 */
	uint32_t link_up;	  /* nic->link_status 快照，0/1 */
	uint32_t link_speed;	  /* Mbps */
	uint8_t mac[6];
	uint8_t _pad[2];

	/* 模块配置参数快照 (由 e1000_stats_snapshot 每 tick 刷新) */
	int32_t rx_mode;   /* 1=simple 2=batch 3=batch_swap 4=single_thr 5=mt */
	int32_t idle_mode; /* 0=schedule(spin) 1=schedule_timeout(sleep) */
	int32_t _pad2;	   /* align to 8 bytes */
};

/*
 * module 加载完成后注册埋点结构地址。当前没有 module unload，传 NULL 不代表
 * 已实现卸载生命周期。注册由加载流程单次调用，monitor 在注册后读取该指针。
 */
void stats_net_register(const struct e1000_stats *p);

#endif

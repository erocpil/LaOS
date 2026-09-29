#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

#include <stdint.h>
#include "e1000.h"

// 以太网帧头 (14字节)
struct eth_header {
	uint8_t  dest[6];
	uint8_t  src[6];
	uint16_t type; // 大端序
} __attribute__((packed));

// ARP 报文 (28字节)
struct arp_packet {
	uint16_t hw_type;      // 硬件类型 (Ethernet = 1)
	uint16_t proto_type;   // 协议类型 (IPv4 = 0x0800)
	uint8_t  hw_addr_len;  // MAC长度 (6)
	uint8_t  proto_addr_len;// IP长度 (4)
	uint16_t opcode;       // 操作码 (1=Req， 2=Reply)
	uint8_t  src_mac[6];
	uint32_t src_ip;       // 大端序
	uint8_t  dest_mac[6];
	uint32_t dest_ip;
} __attribute__((packed));

// IPv4 头部 (20字节)
struct ip_header {
	uint8_t  version_ihl;   // 版本(4位) + 首部长度(4位)
	uint8_t  tos;           // 服务类型
	uint16_t len;           // 总长度
	uint16_t id;            // 标识
	uint16_t flags_offset;  // 标志(3位) + 片偏移(13位)
	uint8_t  ttl;           // 生存时间
	uint8_t  proto;         // 协议 (ICMP=1, TCP=6, UDP=17)
	uint16_t checksum;      // 头部校验和
	uint32_t src_ip;
	uint32_t dest_ip;
} __attribute__((packed));

// ICMP 头部 (8字节)
struct icmp_header {
	uint8_t  type;          // 类型 (Echo Request=8, Echo Reply=0)
	uint8_t  code;          // 代码 (0)
	uint16_t checksum;      // 校验和
	uint16_t id;            // 标识符
	uint16_t seq;           // 序列号
} __attribute__((packed));

// 字节序转换宏
#define swap16(x) ((uint16_t)((((x) >> 8) & 0xff) | (((x) & 0xff) << 8)))
#define swap32(x) ((uint32_t)( \
	(((x) >> 24) & 0xff) | \
	(((x) >>  8) & 0xff00) | \
	(((x) <<  8) & 0xff0000) | \
	(((x) << 24) & 0xff000000)))
#define hton16(x) swap16(x)
#define ntoh16(x) swap16(x)
#define hton32(x) swap32(x)
#define ntoh32(x) swap32(x)

void dump_mac(struct eth_header *eth);
void dump_arp(struct arp_packet *arp);
uint16_t net_checksum(void *data, int len);
void send_arp_reply(e1000_driver_t *nic, struct eth_header *req_eth, struct arp_packet *req_arp);
void send_icmp_reply(e1000_driver_t *nic, struct eth_header *req_eth, struct ip_header *req_ip, struct icmp_header *req_icmp, int payload_len);

#endif

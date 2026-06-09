#include "protocol.h"
#include "../kernel/string.h"
#include "../kernel/printf.h"
#include "../kernel/debug.h"

void send_arp_reply(e1000_driver_t *nic, struct eth_header *req_eth, struct arp_packet *req_arp)
{
	uint8_t response[256]; // 足够容纳以太网帧+ARP
	memset(response, 0, sizeof(response));

	// L("%p %p", req_eth, req_arp);
	struct eth_header *eth = (struct eth_header *)response;
	struct arp_packet *arp = (struct arp_packet *)(response + sizeof(struct eth_header));

	// L("eth->dest %p req_eth->src %p eth->src %p nic->mac %p", eth->dest, req_eth->src, eth->src, nic->mac);
	// 1. 构造以太网头：发回给请求者
	memcpy(eth->dest, req_eth->src, 6);
	memcpy(eth->src, nic->mac, 6);
	eth->type = hton16(0x0806);

	// 2. 构造 ARP Reply 报文
	arp->hw_type = hton16(1);        // Ethernet
	arp->proto_type = hton16(0x0800); // IPv4
	arp->hw_addr_len = 6;
	arp->proto_addr_len = 4;
	arp->opcode = hton16(2);          // Reply

	memcpy(arp->src_mac, nic->mac, 6);
	arp->src_ip = req_arp->dest_ip;   // 既然他在找这个 IP，我们就用这个 IP 回复
	memcpy(arp->dest_mac, req_arp->src_mac, 6);
	arp->dest_ip = req_arp->src_ip;

	// 3. 发送报文
	L("[E1000] Sending ARP Reply...\n");
	nic->tx(nic, response, sizeof(struct eth_header) + sizeof(struct arp_packet));
}

void send_icmp_reply(e1000_driver_t *nic, struct eth_header *req_eth,
		struct ip_header *req_ip, struct icmp_header *req_icmp, int payload_len)
{
	uint8_t response[512];
	const int max_payload_len = sizeof(response) - sizeof(struct eth_header)
		- sizeof(struct ip_header) - sizeof(struct icmp_header);

	/* The request IP length is attacker-controlled; keep the reply on-stack. */
	if (payload_len < 0)
		payload_len = 0;
	else if (payload_len > max_payload_len)
		payload_len = max_payload_len;

	memset(response, 0, sizeof(response));

	struct eth_header *eth = (struct eth_header *)response;
	struct ip_header *ip = (struct ip_header *)(response + sizeof(struct eth_header));
	struct icmp_header *icmp = (struct icmp_header *)(response + sizeof(struct eth_header) + sizeof(struct ip_header));

	// 1. 以太网头
	memcpy(eth->dest, req_eth->src, 6);
	memcpy(eth->src, nic->mac, 6);
	eth->type = hton16(0x0800); // IPv4

	// 2. IP 头
	ip->version_ihl = 0x45; // Version 4, Len 20 bytes
	ip->len = hton16(sizeof(struct ip_header) + sizeof(struct icmp_header) + payload_len);
	ip->id = hton16(ntoh16(req_ip->id) + 1);
	ip->ttl = 64;
	ip->proto = 1; // ICMP
	ip->src_ip = req_ip->dest_ip;
	ip->dest_ip = req_ip->src_ip;
	ip->checksum = 0;
	ip->checksum = net_checksum(ip, sizeof(struct ip_header));

	// 3. ICMP 头 & Payload
	icmp->type = 0; // Echo Reply
	icmp->code = 0;
	icmp->id = req_icmp->id;
	icmp->seq = req_icmp->seq;

	// 拷贝 Ping 包自带的 Payload (比如数据部分)
	if (payload_len > 0) {
		memcpy((uint8_t*)icmp + sizeof(struct icmp_header),
				(uint8_t*)req_icmp + sizeof(struct icmp_header), payload_len);
	}

	icmp->checksum = 0;
	icmp->checksum = net_checksum(icmp, sizeof(struct icmp_header) + payload_len);

	// 4. 发送
	int total_len = sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct icmp_header) + payload_len;
	/*
	kprintf("[ICMP] Sending Echo Reply to %d.%d.%d.%d\n",
			((uint8_t*)&ip->dest_ip)[0], ((uint8_t*)&ip->dest_ip)[1],
			((uint8_t*)&ip->dest_ip)[2], ((uint8_t*)&ip->dest_ip)[3]);
			*/
	nic->tx(nic, response, total_len);
}

// protocol.c
uint16_t net_checksum(void *data, int len)
{
	uint32_t sum = 0;
	uint16_t *ptr = (uint16_t*)data;

	while (len > 1) {
		sum += *ptr++;
		len -= 2;
	}
	if (len > 0) {
		sum += *(uint8_t*)ptr;
	}
	while (sum >> 16) {
		sum = (sum & 0xFFFF) + (sum >> 16);
	}
	return ~((uint16_t)sum);
}

void dump_mac(struct eth_header *eth)
{
	uint16_t type = ntoh16(eth->type);
	kprintf("[ETH] %02x:%02x:%02x:%02x:%02x:%02x -> %02x:%02x:%02x:%02x:%02x:%02x, Type: 0x%04x\n",
			eth->src[0], eth->src[1], eth->src[2], eth->src[3], eth->src[4], eth->src[5],
			eth->dest[0], eth->dest[1], eth->dest[2], eth->dest[3], eth->dest[4], eth->dest[5],
			type);
}

void dump_arp(struct arp_packet *arp)
{
	uint16_t opcode = ntoh16(arp->opcode);
	kprintf("[ARP] %s | Sender: %d.%d.%d.%d | Target: %d.%d.%d.%d\n",
			(opcode == 1) ? "Request" : "Reply",
			((uint8_t*)&arp->src_ip)[0], ((uint8_t*)&arp->src_ip)[1],
			((uint8_t*)&arp->src_ip)[2], ((uint8_t*)&arp->src_ip)[3],
			((uint8_t*)&arp->dest_ip)[0], ((uint8_t*)&arp->dest_ip)[1],
			((uint8_t*)&arp->dest_ip)[2], ((uint8_t*)&arp->dest_ip)[3]);
}

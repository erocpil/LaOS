#!/bin/bash
# setup_host_net.sh {up|down}
#
# 幂等管理 tap0 虚拟网卡，供 QEMU -netdev tap 使用。
# LaOS 内核 e1000 驱动会挂载到该 tap 设备进行数据面遥测。
#
# 用法:
#
#   # 创建 tap0，配置 IP
#   sudo ./script/setup_host_net.sh up
#
#   # 同时打开 NAT，允许 guest 出外网
#   sudo ENABLE_NAT=1 ./script/setup_host_net.sh up
#
#   # 删除 tap0，清理 NAT 规则
#   sudo ./script/setup_host_net.sh down
#
# 环境变量:
#   TAP        tap 设备名（默认 tap0）
#   ADDR       tap CIDR（默认 192.168.56.1/24）
#   UPLINK     NAT 出口网卡（默认自动探测默认路由所在 iface）
#   ENABLE_NAT 设 1 时启用 MASQUERADE + ip_forward

set -e

TAP=${TAP:-tap0}
ADDR=${ADDR:-192.168.56.1/24}
SUBNET=${ADDR%/*}/${ADDR##*/}
USER_ID=${SUDO_USER:-$(id -un)}
UPLINK=${UPLINK:-$(ip route show default 2>/dev/null | awk '/default/ {print $5; exit}')}

if [ "$(id -u)" != "0" ]; then
	echo "error: 需要 root 权限（请 sudo 执行）" >&2
	exit 1
fi

action=${1:-up}

case "$action" in
	up)
		# tap device
		if ! ip link show "$TAP" >/dev/null 2>&1; then
			ip tuntap add dev "$TAP" mode tap user "$USER_ID"
			echo "created tap device: $TAP (owner=$USER_ID)"
		else
			echo "tap device already exists: $TAP"
		fi

		# ip address
		if ! ip addr show dev "$TAP" | grep -q "${ADDR%/*}"; then
			ip addr add "$ADDR" dev "$TAP"
		fi

		ip link set "$TAP" up
		echo "tap up: $TAP $ADDR"

		# NAT
		if [ "${ENABLE_NAT:-0}" = "1" ]; then
			if [ -z "$UPLINK" ]; then
				echo "warning: 未检测到默认路由，跳过 NAT 配置" >&2
			else
				sysctl -w net.ipv4.ip_forward=1 >/dev/null
				if ! iptables -t nat -C POSTROUTING -s "$SUBNET" -o "$UPLINK" -j MASQUERADE 2>/dev/null; then
					iptables -t nat -A POSTROUTING -s "$SUBNET" -o "$UPLINK" -j MASQUERADE
				fi
				if ! iptables -C FORWARD -i "$TAP" -o "$UPLINK" -j ACCEPT 2>/dev/null; then
					iptables -A FORWARD -i "$TAP" -o "$UPLINK" -j ACCEPT
				fi
				if ! iptables -C FORWARD -i "$UPLINK" -o "$TAP" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null; then
					iptables -A FORWARD -i "$UPLINK" -o "$TAP" -m state --state RELATED,ESTABLISHED -j ACCEPT
				fi
				echo "NAT enabled: $SUBNET -> $UPLINK"
			fi
		fi
		;;
	down)
		# clear NAT rules anyway
		if [ -n "$UPLINK" ]; then
			iptables -t nat -D POSTROUTING -s "$SUBNET" -o "$UPLINK" -j MASQUERADE 2>/dev/null || true
			iptables -D FORWARD -i "$TAP" -o "$UPLINK" -j ACCEPT 2>/dev/null || true
			iptables -D FORWARD -i "$UPLINK" -o "$TAP" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
		fi

		# delete tap device
		if ip link show "$TAP" >/dev/null 2>&1; then
			ip link del "$TAP"
			echo "removed tap device: $TAP"
		else
			echo "tap device not present: $TAP"
		fi
		;;
	*)
		echo "Usage: sudo $0 {up|down}" >&2
		exit 1
		;;
esac

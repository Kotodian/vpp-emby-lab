#!/bin/bash
set -u
pkill -TERM -x vpp_main >/dev/null 2>&1 || true
for i in $(seq 1 10); do
  pgrep -x vpp_main >/dev/null 2>&1 || break
  sleep 1
done
pkill -KILL -x vpp_main >/dev/null 2>&1 || true
sleep 1
ip link del vpp-punt >/dev/null 2>&1 || true
/usr/bin/dpdk-devbind.py --bind=igb 0000:02:00.0 >/dev/null 2>&1 || true
ip link set enp2s0f0 up >/dev/null 2>&1 || true
ip addr add 184.107.84.201/26 dev enp2s0f0 >/dev/null 2>&1 || true
ip route add default via 184.107.84.254 dev enp2s0f0 onlink >/dev/null 2>&1 || true
rm -f /run/wan-cutover

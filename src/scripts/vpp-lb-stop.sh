#!/bin/bash
set -u

HOST_IP=194.147.100.232
GW=194.147.100.1
PCI=0000:00:03.0

rm -f /run/vpp-lb/armed
pkill -TERM -x vpp_main >/dev/null 2>&1 || true
for _ in $(seq 1 10); do
  pgrep -x vpp_main >/dev/null 2>&1 || break
  sleep 1
done
pkill -KILL -x vpp_main >/dev/null 2>&1 || true
sleep 1

ip link del vpp-punt >/dev/null 2>&1 || true

if [ -e /sys/bus/pci/devices/$PCI/driver ]; then
  drv=$(basename "$(readlink /sys/bus/pci/devices/$PCI/driver)")
  echo "$PCI" > "/sys/bus/pci/drivers/$drv/unbind" 2>/dev/null || true
fi
echo "$PCI" > /sys/bus/pci/drivers/virtio-pci/bind 2>/dev/null || true

ip link set eth0 up >/dev/null 2>&1 || true
ip addr add "$HOST_IP/24" dev eth0 >/dev/null 2>&1 || true
ip route add default via "$GW" dev eth0 onlink >/dev/null 2>&1 || true
echo "vpp-lb stopped"

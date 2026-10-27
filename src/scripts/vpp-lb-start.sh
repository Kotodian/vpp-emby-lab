#!/bin/bash
set -u

ROOT=/opt/vpp
VPP=$ROOT/bin/vpp
VCTL="$ROOT/bin/vppctl -s /run/vpp-lb/cli.sock"
CONF=/etc/vpp-lb/startup.conf
HOST_IP=194.147.100.232
GW=194.147.100.1
BACKEND=184.107.84.201

export LD_LIBRARY_PATH=/opt/dpdk/lib/x86_64-linux-gnu:$ROOT/lib/x86_64-linux-gnu

modprobe vfio >/dev/null 2>&1 || true
if [ -w /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]; then
  echo 1 > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode 2>/dev/null || true
fi
modprobe vfio-pci >/dev/null 2>&1 || true

mkdir -p /run/vpp-lb /var/log/vpp-lb
ip link set eth0 down >/dev/null 2>&1 || true
setsid nohup "$VPP" -c "$CONF" >/var/log/vpp-lb/stdout 2>&1 </dev/null &

for _ in $(seq 1 30); do
  $VCTL show version >/dev/null 2>&1 && break
  sleep 1
done

if ! $VCTL show version >/dev/null 2>&1; then
  echo "vpp-lb: VPP failed to start" >&2
  /usr/local/sbin/vpp-lb-stop.sh >/dev/null 2>&1 || true
  exit 1
fi

WAN_IF=$($VCTL show interface 2>/dev/null | awk '$1 ~ /^[A-Za-z]/ {print $1}' | grep -vE '^(Name|local0|loop0|punt0)$' | head -n 1)
if [ -z "$WAN_IF" ]; then
  echo "vpp-lb: no WAN interface after DPDK bind" >&2
  /usr/local/sbin/vpp-lb-stop.sh >/dev/null 2>&1 || true
  exit 1
fi

$VCTL set interface state "$WAN_IF" up >/dev/null 2>&1 || true
$VCTL set interface mtu 1500 "$WAN_IF" >/dev/null 2>&1 || true

$VCTL lb conf ip4-src-address "$HOST_IP" buckets 65536 timeout 300 >/dev/null 2>&1 || true
$VCTL lb vip "$HOST_IP/32" encap nat4 port 443 target_port 443 new_len 1024 >/dev/null 2>&1 || true
$VCTL lb as "$HOST_IP/32" "$BACKEND" weight 100 >/dev/null 2>&1 || true
$VCTL lb set interface nat4 in "$WAN_IF" >/dev/null 2>&1 || true

$VCTL create tap id 10 if-name punt0 host-if-name vpp-punt tun host-mtu-size 1500 gso gro-coalesce csum-offload >/dev/null 2>&1 || true
$VCTL set interface state punt0 up >/dev/null 2>&1 || true
$VCTL set interface ip address punt0 10.30.0.1/30 >/dev/null 2>&1 || true
$VCTL set interface mtu 1500 punt0 >/dev/null 2>&1 || true
$VCTL set punt tcp all >/dev/null 2>&1 || true
$VCTL set punt udp all >/dev/null 2>&1 || true
$VCTL ip punt redirect add rx "$WAN_IF" via 10.30.0.2 punt0 >/dev/null 2>&1 || true

ip link set vpp-punt up >/dev/null 2>&1 || true
ip addr add "$HOST_IP/32" dev vpp-punt >/dev/null 2>&1 || true
ip addr add 10.30.0.2/30 dev vpp-punt >/dev/null 2>&1 || true
ip route add default via 10.30.0.1 dev vpp-punt src "$HOST_IP" >/dev/null 2>&1 || true
sysctl -w net.ipv4.conf.vpp-punt.rp_filter=2 >/dev/null 2>&1 || true

pid=$(pgrep -x vpp_main | head -n 1)
[ -n "$pid" ] && echo "$pid" > /run/vpp-lb/vpp.pid
touch /run/vpp-lb/armed
echo "vpp-lb ready"

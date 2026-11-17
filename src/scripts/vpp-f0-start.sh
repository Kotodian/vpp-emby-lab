#!/bin/bash
set -u
ROOT=/root/netsystem-rs/third_party/vpp
if [ -r /etc/default/vpp-f0 ]; then . /etc/default/vpp-f0; fi
VPP_INSTALL_DIR=${VPP_INSTALL_DIR:-$ROOT/build-vpp-lab-install}
VPP=$VPP_INSTALL_DIR/bin/vpp
VCTL="$VPP_INSTALL_DIR/bin/vppctl -s /run/vpp-dpdk/cli.sock"
export LD_LIBRARY_PATH="$VPP_INSTALL_DIR/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

if [ -e /run/vpp-wan-recovery-required ]; then
  echo 'vpp-f0: WAN recovery is still pending; refusing a competing cutover' >&2
  exit 1
fi

ulimit -c unlimited

for module in vfio vfio_iommu_type1 vfio-pci; do
  modprobe "$module" || {
    echo "vpp-f0: failed to load $module" >&2
    exit 1
  }
done

pid=$(cat /run/vpp-dpdk/vpp.pid 2>/dev/null || true)
if ! { [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null &&
       [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = vpp_main ] &&
       timeout 5 $VCTL show version >/dev/null 2>&1; }; then
  ip link set enp2s0f0 down 2>/dev/null || true
  /usr/bin/dpdk-devbind.py --bind=vfio-pci 0000:02:00.0 >/dev/null 2>&1 || true
  setsid nohup $VPP -c /etc/vpp-dpdk/startup-f0.conf >/tmp/vpp-dpdk-f0.stdout 2>&1 </dev/null &
  for i in $(seq 1 30); do
    $VCTL show version >/dev/null 2>&1 && break
    sleep 1
  done
  if ! $VCTL show version >/dev/null 2>&1; then
    echo "vpp-f0: VPP failed to start" >&2
    exit 1
  fi
  if ! $VCTL show interface 2>/dev/null | grep -q 'GigabitEthernet2/0/0'; then
    echo "vpp-f0: GigabitEthernet2/0/0 missing (vfio-pci bind failed)" >&2
    exit 1
  fi
fi

$VCTL create loopback interface instance 0 >/dev/null 2>&1 || true
$VCTL set interface state loop0 up >/dev/null 2>&1 || true
$VCTL set interface ip address loop0 10.255.0.1/32 >/dev/null 2>&1 || true
$VCTL set interface mtu 1500 loop0 >/dev/null 2>&1 || true

$VCTL set interface state GigabitEthernet2/0/0 up >/dev/null 2>&1 || true
$VCTL set interface ip address GigabitEthernet2/0/0 184.107.84.201/26 >/dev/null 2>&1 || true
$VCTL set interface mtu 1500 GigabitEthernet2/0/0 >/dev/null 2>&1 || true
$VCTL ip route add 0.0.0.0/0 via 184.107.84.254 GigabitEthernet2/0/0 >/dev/null 2>&1 || true
$VCTL ip neighbor GigabitEthernet2/0/0 184.107.84.254 00:1c:73:00:00:02 >/dev/null 2>&1 || true

$VCTL create tap id 10 if-name punt0 host-if-name vpp-punt tun host-mtu-size 1500 gso gro-coalesce csum-offload >/dev/null 2>&1 || true
$VCTL set interface state punt0 up >/dev/null 2>&1 || true
$VCTL set interface ip address punt0 10.30.0.1/30 >/dev/null 2>&1 || true
$VCTL set interface mtu packet 1500 punt0 >/dev/null 2>&1 || true
$VCTL set punt tcp all >/dev/null 2>&1 || true
$VCTL set punt udp all >/dev/null 2>&1 || true
$VCTL ip punt redirect add rx GigabitEthernet2/0/0 via 10.30.0.2 punt0 >/dev/null 2>&1 || true

ip link set vpp-punt up
ip addr add 184.107.84.201/32 dev vpp-punt 2>/dev/null || true
ip addr add 10.30.0.2/30 dev vpp-punt 2>/dev/null || true
ip route add default via 10.30.0.1 dev vpp-punt src 184.107.84.201 2>/dev/null || true
sysctl -w net.ipv4.conf.vpp-punt.rp_filter=2 >/dev/null 2>&1 || true

# Docker MASQUERADE picks vpp-punt's 10.30.0.2 instead of the public address,
# so container traffic leaves with an unroutable source. Pin it explicitly.
for net in 172.17.0.0/16 172.18.0.0/16; do
  iptables -t nat -D POSTROUTING -s "$net" -o vpp-punt -j SNAT --to-source 184.107.84.201 2>/dev/null || true
  iptables -t nat -I POSTROUTING 1 -s "$net" -o vpp-punt -j SNAT --to-source 184.107.84.201
done

resolvectl dns vpp-punt 23.19.53.53 23.19.52.52 >/dev/null 2>&1 || true
resolvectl domain vpp-punt '~.' >/dev/null 2>&1 || true
resolvectl default-route vpp-punt yes >/dev/null 2>&1 || true

$VCTL http static server www-root /srv/media cache-size 64m max-age 0 >/dev/null 2>&1 || true
$VCTL http static listener add uri tcp://10.255.0.1/8080 www-root /srv/media cache-size 64m url-handlers >/dev/null 2>&1 || true

bash /usr/local/sbin/vpp-emby-dnat.sh >/dev/null 2>&1 || true
connected=0
for ((attempt=0; attempt<3; attempt++)); do
  for target in 1.1.1.1:443 8.8.8.8:53 9.9.9.9:53; do
    if timeout 3 bash -c 'exec 3<>/dev/tcp/"$1"/"$2"' _ "${target%:*}" "${target#*:}" 2>/dev/null; then
      connected=1
      break
    fi
  done
  [ "$connected" = 1 ] && break
  sleep 1
done
if [ "$connected" != 1 ]; then
  echo 'vpp-f0: WAN connectivity failed; requesting kernel recovery' >&2
  touch /run/vpp-wan-recovery-required
  exit 1
fi
touch /run/wan-cutover
echo "vpp-f0 ready"

#!/bin/bash
set -u

ROOT=/root/netsystem-rs/third_party/vpp
VCTL="$ROOT/build-vpp-lab-install/bin/vppctl -s /run/vpp-dpdk/cli.sock"
LOG=/var/log/vpp-start-guard.log

log() { echo "$(date -Is) $*" >> "$LOG"; }

rollback() {
  log "guard: rollback"
  systemctl stop nginx-vcl.service >/dev/null 2>&1 || true
  pkill -9 -x vpp_main >/dev/null 2>&1 || true
  sleep 1
  ip link del vpp-punt >/dev/null 2>&1 || true
  /usr/bin/dpdk-devbind.py --bind=igb 0000:02:00.0 >/dev/null 2>&1 || true
  ip link set enp2s0f0 up >/dev/null 2>&1 || true
  ip addr add 184.107.84.201/26 dev enp2s0f0 >/dev/null 2>&1 || true
  ip route add default via 184.107.84.254 dev enp2s0f0 onlink >/dev/null 2>&1 || true
  rm -f /run/wan-cutover
  log "guard: rollback done"
}

if ! pgrep -x vpp_main >/dev/null 2>&1; then
  rollback
  exit 0
fi

if ! $VCTL show version >/dev/null 2>&1; then
  rollback
  exit 0
fi

if ! timeout 3 bash -c '</dev/tcp/184.107.84.201/22' >/dev/null 2>&1; then
  rollback
  exit 0
fi

log "guard: vpp and ssh ok"

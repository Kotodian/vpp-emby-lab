#!/bin/bash
set -euo pipefail
ROOT=/root/netsystem-rs/third_party/vpp
LIBDIR=/var/lib/qbit-vcl/current/lib
CONFIG=/etc/vpp/qbittorrent-vcl.conf

if [ -e /run/vpp-wan-recovery-required ] || [ ! -e /run/wan-cutover ]; then
  echo 'qbit-vcl: WAN is not ready; refusing kernel fallback' >&2
  exit 1
fi
systemctl is-active --quiet wan-watchdog.service
timeout 5 "$ROOT/build-vpp-lab-install/bin/vppctl" -s /run/vpp-dpdk/cli.sock show version >/dev/null
test -r "$LIBDIR/libvcl_ldpreload.so.27.02"
test -r "$CONFIG"
ulimit -c unlimited
exec env LD_PRELOAD="$LIBDIR/libvcl_ldpreload.so.27.02" \
  LD_LIBRARY_PATH="$LIBDIR" VCL_CONFIG="$CONFIG" \
  LDP_APP_NAME=qbittorrent LDP_DEBUG=0 VCL_DEBUG=0 \
  /usr/bin/qbittorrent-nox "$@"

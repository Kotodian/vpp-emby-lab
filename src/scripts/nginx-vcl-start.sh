#!/bin/bash
set -euo pipefail

ROOT=${VPP_ROOT:-/root/netsystem-rs/third_party/vpp}
VCTL="$ROOT/build-vpp-lab-install/bin/vppctl"
LD_PRELOAD_LIB="$ROOT/build-vpp-lab-install/lib/x86_64-linux-gnu/libvcl_ldpreload.so"
VCL_CONFIG=${VCL_CONFIG:-/etc/vpp/vcl.conf}

if [[ ! -r "$LD_PRELOAD_LIB" ]]; then
  echo "nginx-vcl: missing $LD_PRELOAD_LIB" >&2
  exit 1
fi

if ! "$VCTL" -s /run/vpp-dpdk/cli.sock show version >/dev/null 2>&1; then
  echo "nginx-vcl: VPP is not reachable; refusing to start nginx under VCL" >&2
  exit 1
fi

export LD_PRELOAD="$LD_PRELOAD_LIB"
export VCL_CONFIG
exec nginx -c /etc/nginx/pornvpp.conf

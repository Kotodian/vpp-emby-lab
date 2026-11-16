#!/bin/bash
set -euo pipefail
# VPP cut-through peers have address 0.0.0.0:0. To retain qbit's original
# local-auth bypass, whitelist exactly 0.0.0.0/32 on its 127.0.0.1 WebUI.
LIBDIR=/var/lib/qbit-vcl/current/lib
exec env LD_PRELOAD="$LIBDIR/libvcl_ldpreload.so.27.02" \
  LD_LIBRARY_PATH="$LIBDIR" VCL_CONFIG=/etc/vpp/qbittorrent-vcl.conf \
  LDP_APP_NAME=qbit-webui LDP_DEBUG=0 VCL_DEBUG=0 \
  /var/lib/qbit-vcl/current/bin/vcl_socket_proxy 127.0.0.1 8080

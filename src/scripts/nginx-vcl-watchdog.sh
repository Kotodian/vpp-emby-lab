#!/bin/bash
set -u

VCTL=/root/netsystem-rs/third_party/vpp/build-vpp-lab-install/bin/vppctl
LOG=/var/log/nginx-vcl-watchdog.log
ERROR_LOG=/var/log/nginx/pornvpp-error.log
MAX_ERROR_LOG=$((100 * 1024 * 1024))

log() { echo "$(date -Is) $*" >> "$LOG"; }

misses=0
while true; do
  if ! systemctl is-active --quiet nginx-vcl.service; then
    misses=0
    sleep 5
    continue
  fi

  if ! "$VCTL" -s /run/vpp-dpdk/cli.sock show version >/dev/null 2>&1; then
    log "VPP unreachable; stopping nginx-vcl to avoid epoll busy loop"
    systemctl stop nginx-vcl.service >/dev/null 2>&1 || true
    misses=0
    sleep 10
    continue
  fi

  if ! "$VCTL" -s /run/vpp-dpdk/cli.sock show session listeners tcp 2>/dev/null |
       grep -q '184.107.84.201:443'; then
    misses=$((misses + 1))
    if [ "$misses" -ge 2 ]; then
      log "443 listener missing; restarting nginx-vcl"
      systemctl restart nginx-vcl.service >/dev/null 2>&1 || true
      misses=0
      sleep 3
    fi
  else
    misses=0
  fi

  if [ -f "$ERROR_LOG" ] && [ "$(stat -c %s "$ERROR_LOG" 2>/dev/null || echo 0)" -gt "$MAX_ERROR_LOG" ]; then
    log "nginx error log exceeded 100MB; truncating and restarting nginx-vcl"
    truncate -s 0 "$ERROR_LOG" 2>/dev/null || true
    systemctl restart nginx-vcl.service >/dev/null 2>&1 || true
    sleep 3
  fi

  sleep 5
done

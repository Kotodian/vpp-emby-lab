#!/bin/bash
set -u

ROOT=/opt/vpp
VCTL="$ROOT/bin/vppctl -s /run/vpp-lb/cli.sock"
HOST_IP=194.147.100.232
MARKER=/run/vpp-lb/armed
LOG=/var/log/vpp-lb-watchdog.log

log() { echo "$(date -Is) $*" >> "$LOG"; }

while true; do
  if [ -f "$MARKER" ]; then
    if ! pgrep -x vpp_main >/dev/null 2>&1 || ! $VCTL show version >/dev/null 2>&1; then
      log "VPP unavailable; rolling back eth0"
      /usr/local/sbin/vpp-lb-stop.sh >/dev/null 2>&1 || true
    elif ! timeout 2 bash -c "</dev/tcp/$HOST_IP/22" >/dev/null 2>&1; then
      log "SSH punt unavailable; rolling back eth0"
      /usr/local/sbin/vpp-lb-stop.sh >/dev/null 2>&1 || true
    fi
  fi
  sleep 10
done

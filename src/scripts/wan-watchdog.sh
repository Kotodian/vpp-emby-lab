#!/bin/bash
# This watchdog is independent of VPP. It restores the host, never qbit's
# kernel dataplane. Restarting the watchdog must not kill its existing VPP child.
set -uo pipefail
ROOT=/root/netsystem-rs/third_party/vpp
VCTL=$ROOT/build-vpp-lab-install/bin/vppctl
MARKER=/run/wan-cutover
LOG=/var/log/wan-watchdog.log
FAILS=0
log() { printf '%s %s\n' "$(date -Is)" "$*" >> "$LOG"; }

net_ok() {
  ip route get 1.1.1.1 2>/dev/null | grep -q 'dev vpp-punt' || return 1
  local target
  for target in 1.1.1.1:443 8.8.8.8:53 9.9.9.9:53; do
    timeout 3 bash -c 'exec 3<>/dev/tcp/"$1"/"$2"' _ "${target%:*}" "${target#*:}" >/dev/null 2>&1 && return 0
  done
  return 1
}

rollback() {
  log "VPP unavailable: stopping qbit and restoring the WAN to Linux"
  touch /run/vpp-wan-recovery-required
  # Explicit stop suppresses VPP's Restart= policy while recovery runs.
  systemctl stop qbittorrent.service nginx-vcl.service >> "$LOG" 2>&1 || return 1
  systemctl stop vpp-f0.service >> "$LOG" 2>&1 || log "VPP service stop failed; retrying recovery directly"
  /usr/local/sbin/vpp-f0-stop.sh >> "$LOG" 2>&1
}

log "core-preserving WAN watchdog started"
while :; do
  if [ -f /run/vpp-wan-recovery-required ]; then
    rollback || log "recovery failed; will retry; see /var/log/vpp-wan-recovery.log"
  elif [ -f "$MARKER" ]; then
    pid=$(cat /run/vpp-dpdk/vpp.pid 2>/dev/null || true)
    if [[ ! "$pid" =~ ^[0-9]+$ ]] || ! kill -0 "$pid" 2>/dev/null; then
      rollback || log "recovery failed; will retry"
    elif ! timeout 3 "$VCTL" -s /run/vpp-dpdk/cli.sock show version >/dev/null 2>&1; then
      FAILS=$((FAILS+1))
      log "VPP CLI probe failed ($FAILS/2)"
      if [ "$FAILS" -ge 2 ]; then rollback || log "recovery failed; will retry"; fi
    elif net_ok; then
      FAILS=0
    else
      FAILS=$((FAILS+1))
      log "VPP WAN probe failed ($FAILS/2)"
      if [ "$FAILS" -ge 2 ]; then rollback || log "recovery failed; will retry"; fi
    fi
  else
    FAILS=0
  fi
  sleep 5
done

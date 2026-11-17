#!/bin/bash
# Return this WAN device to Linux; never interrupt a core being written.
set -euo pipefail

PCI=0000:02:00.0
ADDRESS=184.107.84.201/26
GATEWAY=184.107.84.254
LOG=/var/log/vpp-wan-recovery.log
PIDFILE=/run/vpp-dpdk/vpp.pid

exec 9>/run/vpp-wan-recovery.lock
flock 9
exec >>"$LOG" 2>&1
log() { printf '%s %s\n' "$(date -Is)" "$*"; }
trap 'rc=$?; log "FAILED rc=$rc line=$LINENO; recovery remains required"; touch /run/vpp-wan-recovery-required; exit "$rc"' ERR
run() { log "command: $*"; "$@"; }
write_sysfs() { log "sysfs: $1 <- $2"; printf '%s\n' "$2" > "$1"; }

log "WAN recovery started"
# Stop applications first. They must never restart on the restored kernel path.
for unit in qbittorrent.service nginx-vcl.service; do
  if systemctl is-active --quiet "$unit"; then
    run systemctl stop "$unit"
  fi
done

pid=$(cat "$PIDFILE" 2>/dev/null || true)
alive() {
  [[ "$pid" =~ ^[0-9]+$ ]] && [ -r "/proc/$pid/status" ] &&
    [ "$(cat "/proc/$pid/comm" 2>/dev/null)" = vpp_main ] &&
    ! awk '$1 == "State:" && $2 == "Z" { zombie = 1 } END { exit !zombie }' "/proc/$pid/status"
}
dumping() { awk '$1 == "CoreDumping:" && $2 == 1 { found = 1 } END { exit !found }' "/proc/$pid/status" 2>/dev/null; }
wait_core() {
  while alive && dumping; do
    log "pid=$pid is writing core; preserving it before PCI rebind"
    sleep 5
  done
}

if alive; then
  wait_core
  if alive; then
    run kill -TERM "$pid"
    for ((attempt=0; attempt<10; attempt++)); do
      alive || break
      sleep 1
    done
    wait_core
  fi
  if alive; then
    # A stuck process gets an ABRT diagnostic, not an evidence-destroying KILL.
    log "pid=$pid did not exit; requesting SIGABRT core"
    run kill -ABRT "$pid"
    for ((attempt=0; attempt<30; attempt++)); do
      alive || break
      sleep 1
      wait_core
    done
  fi
  if alive; then
    log "pid=$pid still alive; refusing unsafe PCI rebind"
    false
  fi
fi

run modprobe igb
device=/sys/bus/pci/devices/$PCI
current=$(basename "$(readlink "$device/driver")")
if [ "$current" != igb ]; then
  write_sysfs "$device/driver_override" igb
  if [ -e "$device/driver/unbind" ]; then
    write_sysfs "$device/driver/unbind" "$PCI"
  fi
  write_sysfs /sys/bus/pci/drivers_probe "$PCI"
  write_sysfs "$device/driver_override" ''
fi
[ "$(basename "$(readlink "$device/driver")")" = igb ]

# igb initially publishes eth0; udev can rename it while addresses and
# routes are being restored. Wait before retaining the interface name.
if command -v udevadm >/dev/null 2>&1; then
  run udevadm settle --timeout=10
fi
iface=''
for ((attempt=0; attempt<20; attempt++)); do
  for path in "$device"/net/*; do
    if [ -e "$path" ]; then iface=${path##*/}; break; fi
  done
  [ -n "$iface" ] && break
  sleep 1
done
[ -n "$iface" ]
run ip link set "$iface" up
for ((attempt=0; attempt<20; attempt++)); do
  [ "$(cat "/sys/class/net/$iface/carrier" 2>/dev/null)" = 1 ] && break
  sleep 0.25
done
run ip address replace "$ADDRESS" dev "$iface"
run ip route replace default via "$GATEWAY" dev "$iface" onlink src "${ADDRESS%/*}"
run ip neighbor replace "$GATEWAY" lladdr 00:1c:73:00:00:02 nud permanent dev "$iface"
if ip link show vpp-punt >/dev/null 2>&1; then
  run ip link delete vpp-punt
fi
run resolvectl dns "$iface" 23.19.53.53 23.19.52.52
run resolvectl domain "$iface" '~.'
run resolvectl default-route "$iface" yes

connected=0
for target in 1.1.1.1:443 8.8.8.8:53 9.9.9.9:53; do
  if timeout 3 bash -c 'exec 3<>/dev/tcp/"$1"/"$2"' _ "${target%:*}" "${target#*:}"; then
    connected=1
    break
  fi
done
[ "$connected" = 1 ]
ip route get 1.1.1.1 | grep -q "dev $iface"
rm -f /run/wan-cutover /run/vpp-wan-recovery-required
log "WAN recovered: PCI=$PCI driver=igb interface=$iface; qbit remains stopped"

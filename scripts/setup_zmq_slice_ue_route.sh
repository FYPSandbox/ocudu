#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: sudo $0 <ue1|ue2|ue3>"
}

if [[ $# -ne 1 ]]; then
  usage
  exit 2
fi

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi

case "$1" in
  ue1) NS=ue1; UE_IP=10.45.1.2 ;;
  ue2) NS=ue2; UE_IP=10.45.2.2 ;;
  ue3) NS=ue3; UE_IP=10.45.3.2 ;;
  *) usage; exit 2 ;;
esac

if ! ip netns list | awk '{print $1}' | grep -Fxq "$NS"; then
  echo "Namespace $NS does not exist. Attach $NS first." >&2
  exit 1
fi

if ! ip netns exec "$NS" ip link show tun_srsue >/dev/null 2>&1; then
  echo "tun_srsue is not present in $NS. Wait for UE attachment." >&2
  exit 1
fi

ip netns exec "$NS" ip link set lo up
ip netns exec "$NS" ip route replace default dev tun_srsue
ip route replace "${UE_IP}/32" via 10.53.1.2

echo "Routes installed for $NS ($UE_IP). Testing the host endpoint..."
ip netns exec "$NS" ping -I tun_srsue -c 4 10.53.1.1

echo

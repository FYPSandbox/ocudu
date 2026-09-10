#!/usr/bin/env bash
set -euo pipefail

usage() {
  echo "Usage: sudo $0 <ue1|ue2|ue3> [rate] [seconds]"
  echo "Example: sudo $0 ue2 10M 60"
}

if [[ $# -lt 1 || $# -gt 3 ]]; then
  usage
  exit 2
fi

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script with sudo." >&2
  exit 1
fi

case "$1" in
  ue1|ue2|ue3) NS="$1" ;;
  *) usage; exit 2 ;;
esac

RATE="${2:-20M}"
SECONDS="${3:-60}"

if ! [[ "$SECONDS" =~ ^[1-9][0-9]*$ ]]; then
  echo "Seconds must be a positive integer." >&2
  exit 2
fi

echo "Generating downlink UDP traffic for $NS at $RATE for ${SECONDS}s."
exec ip netns exec "$NS" \
  iperf3 -c 10.53.1.1 -p 5201 \
  -R -u -b "$RATE" -t "$SECONDS" -i 1 \

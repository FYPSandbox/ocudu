#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRSUE_BIN="${SRSUE_BIN:-/home/thilina/FYP/srsRAN_4G/build/srsue/src/srsue}"
BASE_CONFIG="${BASE_CONFIG:-${ROOT_DIR}/configs/ue_zmq.conf}"

usage() {
  echo "Usage: sudo $0 <ue1|ue2|ue3>"
  echo
  echo "  ue1: SST 1, DNN embb,  expected IP 10.45.1.2"
  echo "  ue2: SST 2, DNN mmtc,  expected IP 10.45.2.2"
  echo "  ue3: SST 3, DNN urllc, expected IP 10.45.3.2"
}

if [[ $# -ne 1 ]]; then
  usage
  exit 2
fi

if [[ ${EUID} -ne 0 ]]; then
  echo "Run this script with sudo so srsUE can create the TUN device and namespace." >&2
  exit 1
fi

case "$1" in
  ue1)
    IMSI=001010123456780
    KEY=00112233445566778899aabbccddeeff
    SST=1
    DNN=embb
    NS=ue1
    IP=10.45.1.2
    ;;
  ue2)
    IMSI=001010123456781
    KEY=00112233445566778899aabbccddef00
    SST=2
    DNN=mmtc
    NS=ue2
    IP=10.45.2.2
    ;;
  ue3)
    IMSI=001010123456782
    KEY=00112233445566778899aabbccddef01
    SST=3
    DNN=urllc
    NS=ue3
    IP=10.45.3.2
    ;;
  *)
    usage
    exit 2
    ;;
esac

echo "Starting $1: IMSI=$IMSI SST=$SST SD=1 DNN=$DNN namespace=$NS expected_ip=$IP"
echo "Direct ZMQ supports one active UE process at a time; stop the current UE before switching."

if ! ip netns list | awk '{print $1}' | grep -Fxq "$NS"; then
  echo "Creating network namespace $NS"
  ip netns add "$NS"
fi
ip netns exec "$NS" ip link set lo up

exec "$SRSUE_BIN" \
  --usim.imsi "$IMSI" \
  --usim.k "$KEY" \
  --nas.apn "$DNN" \
  --nas.force_imsi_attach true \
  --slicing.enable true \
  --slicing.nssai-sst "$SST" \
  --slicing.nssai-sd 1 \
  --gw.netns "$NS" \
  --gw.ip_devname tun_srsue \
  --log.filename "/tmp/${NS}.log" \
  --pcap.mac_nr_filename "/tmp/${NS}_mac_nr.pcap" \
  --pcap.nas_filename "/tmp/${NS}_nas.pcap" \
  "$BASE_CONFIG"

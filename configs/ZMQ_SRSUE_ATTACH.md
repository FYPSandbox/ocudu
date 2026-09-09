# srsUE to OCUDU attachment over ZeroMQ

This setup replaces the RF hardware and radio channel with two local TCP IQ
streams. The UE is the `srsue` executable from the separate `srsRAN_4G`
project. OCUDU supplies the gNB and the matching configuration pair.

## Compatibility contract

| Setting | OCUDU gNB | srsUE |
|---|---|---|
| NR band | 3 | 3 |
| Duplex | FDD | FDD, derived from band 3 |
| Channel bandwidth | 10 MHz | Detected from the cell |
| Common SCS | 15 kHz | Detected from the cell |
| Base sample rate | 11.52 MS/s | 11.52 MS/s |
| CORESET0 index | 6 | Decoded from the MIB |
| Downlink IQ | TX server on TCP 2000 | RX client to TCP 2000 |
| Uplink IQ | RX client to TCP 2001 | TX server on TCP 2001 |
| PLMN | 001/01 | IMSI prefix 00101 |
| TAC | 7 | Learned from the cell |
| Default slice | SST 1 | Requested through registration |

The gNB profile sets `cu_cp.inactivity_timer` to 7200 seconds. This keeps an
idle test UE and its GTP-U tunnel alive during manual traffic tests.

The UE uses `continuous_tx = yes`. OCUDU's ZMQ RU operates in blocking mode
and needs a continuous uplink sample stream while the UE searches for SIB1
and before it sends PRACH.

The soft-USIM values in `ue_zmq.conf` match the default subscriber in
`docker/open5gs/open5gs.env`:

- IMSI: `001010123456780`
- K: `00112233445566778899aabbccddeeff`
- OPc: `63bfa50ee6523365ff14c1f45f88737d`
- DNN/APN: `srsapn`
- Assigned address: `10.45.1.2`

If the subscriber database is overridden, update the UE IMSI, K, OPc and APN
to match it.

## Prerequisites

- Build OCUDU with ZeroMQ enabled and confirm CMake reports `ZEROMQ_FOUND`.
- Build `srsRAN_4G` with ZeroMQ enabled and confirm the srsUE ZMQ RF plugin is
  available.
- Run the gNB and UE on the host. Their ZMQ endpoints use `127.0.0.1`, so they
  must share a network namespace.

## Start and verify

Start only the containerized core from the OCUDU repository:

```bash
cd docker
docker compose up -d 5gc
docker compose ps
```

Create the UE namespace once; an `already exists` response is harmless:

```bash
sudo ip netns add ue1
```

Start the OCUDU gNB from the repository root:

```bash
sudo ./build/apps/gnb/gnb -c configs/gnb_zmq_srsue.yml
```

Start the external srsUE in another terminal:

```bash
sudo /path/to/srsRAN_4G/build/srsue/src/srsue \
  /path/to/ocudu/configs/ue_zmq.conf
```

Successful operation has four checkpoints:

1. `/tmp/ocudu_gnb_zmq.log` reports successful NG Setup with the AMF.
2. `/tmp/ue.log` reports cell search, random access and RRC connection.
3. The UE reports successful 5G registration and PDU-session setup.
4. `tun_srsue` exists in `ue1` and can reach the UPF gateway.

```bash
sudo ip netns exec ue1 ip address show tun_srsue
sudo ip netns exec ue1 ip route
sudo ip netns exec ue1 ping -I tun_srsue -c 4 10.45.1.1
```

For failure isolation:

```bash
grep -E "NG Setup|RACH|RRC|PDU|Registration|error|failed" \
  /tmp/ocudu_gnb_zmq.log /tmp/ue.log
```

This baseline intentionally does not enable E2. First verify UE attachment and
the user plane. Add E2 and slicing as a separate overlay so RIC connectivity
cannot hide an RF or registration problem.

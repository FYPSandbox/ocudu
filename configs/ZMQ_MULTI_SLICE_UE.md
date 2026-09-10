# Multi-slice ZMQ UE test workflow

This setup defines three subscriber profiles:

| UE | IMSI | SST/SD | DNN | Static IP | Namespace |
|---|---|---|---|---|---|
| ue1 | 001010123456780 | 1/1 | embb | 10.45.1.2 | ue1 |
| ue2 | 001010123456781 | 2/1 | mmtc | 10.45.2.2 | ue2 |
| ue3 | 001010123456782 | 3/1 | urllc | 10.45.3.2 | ue3 |

## Important radio limitation

The direct OCUDU-to-srsUE ZMQ link on TCP ports 2000/2001 is point-to-point.
Run only one srsUE profile at a time. The profiles let you switch slices
quickly and validate each DNN/S-NSSAI path. Simultaneous UEs require a
timestamp-aware IQ fan-out/combiner or real RF UEs; merely assigning additional
ZMQ ports does not create a shared radio channel.

## Apply the core changes

The Open5GS Python provisioning code and YAML are copied into its image, so
rebuild the 5GC after pulling or changing this setup:

```bash
cd /home/thilina/FYP/team-repos/ocudu
sudo docker compose -f docker/docker-compose.yml down
sudo docker compose -f docker/docker-compose.yml build 5gc
sudo docker compose -f docker/docker-compose.yml up -d 5gc
```

Wait until it is healthy before starting the gNB.

## Start a selected UE

Start the RIC, 5GC, gNB, and xApp first. Start the xApp with
`--cell_prbs 52 --tdd_dl_fraction 0.5` for the current srsUE scheduling
workaround. Then choose exactly one UE:

```bash
cd /home/thilina/FYP/team-repos/ocudu
sudo ./scripts/run_zmq_slice_ue.sh ue1
```

Replace `ue1` with `ue2` or `ue3` to select another slice. Stop the
current UE with Ctrl+C before switching.

After attachment, restore its routes:

```bash
sudo ./scripts/setup_zmq_slice_ue_route.sh ue1
```

Start an iperf server in another terminal:

```bash
iperf3 -s -B 10.53.1.1 -p 5201
```

Generate downlink traffic, optionally selecting rate and duration:

```bash
sudo ./scripts/run_zmq_slice_traffic.sh ue1 20M 60
```

Use the same UE name for launching, routing, and traffic.

## Switch slices

1. Stop traffic and the current UE with Ctrl+C.
2. Keep the RIC, 5GC, gNB, xApp, and iperf server running.
3. Start the next UE profile.
4. Run its route helper after attachment.
5. Run its traffic helper.

If the dashboard does not track the new UE, stop the UE, restart the xApp,
wait for it to become ready, and attach the selected UE again.

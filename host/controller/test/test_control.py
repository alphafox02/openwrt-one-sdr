#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Exercises the VITA 49.2 control responder (vita49-control) the way
# SoapyVITA49 does: a probe (query-state), single-field setField commands, and
# a dry run. Command packets are built and acknowledgements parsed with the
# byte layout of SoapyVITA49's Control.cpp/VRT.cpp. Passing checks this subset,
# not standards conformance or live hardware interoperability. Starts the daemon
# on a loopback port, records what
# the apply hook was asked to do, and checks the acks and the applied values.
import os, socket, struct, subprocess, sys, tempfile, time

PKT_COMMAND = 0x6
HDR_ACKNOWLEDGE = 1 << 26
CAM_CONTROLLEE_ENABLE = 1 << 31
CAM_CONTROLLER_ENABLE = 1 << 29
CAM_ACTION_SHIFT = 23
CAM_REQ_EXECUTION = 1 << 19
CAM_REQ_QUERY_STATE = 1 << 18
CAM_REQ_WARNINGS = 1 << 17
CAM_REQ_ERRORS = 1 << 16
CAM_PERMIT_WARNINGS = 1 << 26
CAM_PERMIT_ERRORS = 1 << 25
ACTION_NONE, ACTION_DRY_RUN, ACTION_EXECUTE = 0, 1, 2

CIF0_BANDWIDTH = 1 << 29
CIF0_RF_REF_FREQ = 1 << 27
CIF0_GAIN = 1 << 23
CIF0_SAMPLE_RATE = 1 << 21
RADIX_FREQ = RADIX_RATE = RADIX_BW = 20
RADIX_GAIN = 7

STREAM_ID = 0x0A0B0C0D
CONTROLLER_ID = 0x11223344


def fixed64(value, radix):
    return struct.pack(">q", int(value * (1 << radix)))


def build_command(message_id, cam, controller_id, fields):
    # fields: list of (cif0_bit, encoded_bytes) in MSB-first order.
    words = [struct.pack(">I", STREAM_ID), struct.pack(">I", cam),
             struct.pack(">I", message_id), struct.pack(">I", STREAM_ID)]
    if controller_id:
        words.append(struct.pack(">I", controller_id))
    cif0 = 0
    body = b""
    for bit, enc in fields:
        cif0 |= bit
        body += enc
    words.append(struct.pack(">I", cif0))
    words.append(body)
    payload = b"".join(words)
    size = (4 + len(payload)) // 4
    hdr = (PKT_COMMAND << 28) | size
    return struct.pack(">I", hdr) + payload


def build_cam(action, want_ack, query, controller_id):
    cam = CAM_CONTROLLEE_ENABLE
    if controller_id:
        cam |= CAM_CONTROLLER_ENABLE
    if query:
        cam |= (ACTION_NONE << CAM_ACTION_SHIFT) | CAM_REQ_QUERY_STATE
    else:
        cam |= (action << CAM_ACTION_SHIFT) | CAM_PERMIT_WARNINGS | CAM_PERMIT_ERRORS
        if want_ack:
            cam |= CAM_REQ_EXECUTION | CAM_REQ_WARNINGS | CAM_REQ_ERRORS
    return cam


def parse_ack(buf, want_query):
    assert len(buf) >= 16, "ack too short"
    (w0,) = struct.unpack(">I", buf[:4])
    assert (w0 >> 28) & 0xF == PKT_COMMAND, "ack is not a command packet"
    assert w0 & HDR_ACKNOWLEDGE, "acknowledge bit not set"
    size = w0 & 0xFFFF
    assert size * 4 == len(buf), f"declared size {size} != {len(buf)//4} words"
    w = 1
    (stream_id,) = struct.unpack(">I", buf[w * 4:w * 4 + 4]); w += 1
    assert stream_id == STREAM_ID, "stream id not echoed"
    (cam,) = struct.unpack(">I", buf[w * 4:w * 4 + 4]); w += 1
    (message_id,) = struct.unpack(">I", buf[w * 4:w * 4 + 4]); w += 1
    if cam & CAM_CONTROLLEE_ENABLE:
        w += 1
    if cam & CAM_CONTROLLER_ENABLE:
        w += 1
    fields = {}
    if w < size:
        (cif0,) = struct.unpack(">I", buf[w * 4:w * 4 + 4]); w += 1
        if cif0 & CIF0_BANDWIDTH:
            (raw,) = struct.unpack(">q", buf[w * 4:w * 4 + 8]); w += 2
            fields["bandwidth"] = raw / (1 << RADIX_BW)
        if cif0 & CIF0_RF_REF_FREQ:
            (raw,) = struct.unpack(">q", buf[w * 4:w * 4 + 8]); w += 2
            fields["rf_freq"] = raw / (1 << RADIX_FREQ)
        if cif0 & CIF0_GAIN:
            (g,) = struct.unpack(">I", buf[w * 4:w * 4 + 4]); w += 1
            fields["gain"] = struct.unpack(">h", struct.pack(">H", g & 0xFFFF))[0] / (1 << RADIX_GAIN)
        if cif0 & CIF0_SAMPLE_RATE:
            (raw,) = struct.unpack(">q", buf[w * 4:w * 4 + 8]); w += 2
            fields["sample_rate"] = raw / (1 << RADIX_RATE)
    return message_id, cam, fields


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    daemon = os.path.join(here, os.pardir, "vita49-control")
    if not os.path.exists(daemon):
        print("build first: cc -O2 -o host/controller/vita49-control "
              "host/controller/vita49-control.c", file=sys.stderr)
        return 1

    tmp = tempfile.mkdtemp()
    log = os.path.join(tmp, "applied.log")
    hook = os.path.join(tmp, "apply.sh")
    with open(hook, "w") as f:
        f.write('#!/bin/sh\n[ "$1" = --check ] && exit 0\necho "$1 $2" >> ' + log + "\n")
    os.chmod(hook, 0o755)

    port = 54991
    proc = subprocess.Popen([daemon, "--addr", "127.0.0.1", "--port", str(port),
                             "--apply", hook, "--rf-freq", "2437000000",
                             "--sample-rate", "40000000"],
                            stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.3)
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.settimeout(1.0)
        sock.connect(("127.0.0.1", port))
        mid = 1

        def exchange(cam, fields, want_query):
            nonlocal mid
            pkt = build_command(mid, cam, CONTROLLER_ID, fields)
            sock.send(pkt)
            ack = sock.recv(65536)
            msg, acam, f = parse_ack(ack, want_query)
            assert msg == mid, f"message id {msg} != {mid}"
            mid += 1
            return f

        # 1. Probe: query-state, no change. SoapyVITA49 sends the four field
        # bits with zeroed bodies; the controllee fills them in. Reports the
        # daemon's defaults.
        probe_fields = [
            (CIF0_BANDWIDTH, fixed64(0, RADIX_BW)),
            (CIF0_RF_REF_FREQ, fixed64(0, RADIX_FREQ)),
            (CIF0_GAIN, struct.pack(">I", 0)),
            (CIF0_SAMPLE_RATE, fixed64(0, RADIX_RATE)),
        ]
        f = exchange(build_cam(ACTION_NONE, True, True, CONTROLLER_ID), probe_fields, True)
        assert abs(f["rf_freq"] - 2437000000) < 1, f
        assert abs(f["sample_rate"] - 40000000) < 1, f
        print("probe ok:", {k: round(v) for k, v in f.items()})

        # 2. setField(RF_REF_FREQ) execute.
        f = exchange(build_cam(ACTION_EXECUTE, True, False, CONTROLLER_ID),
                     [(CIF0_RF_REF_FREQ, fixed64(2412000000, RADIX_FREQ))], False)
        assert abs(f["rf_freq"] - 2412000000) < 1, f
        print("tune ack ok:", round(f["rf_freq"]))

        # 3. setField(SAMPLE_RATE) and setField(GAIN) execute.
        exchange(build_cam(ACTION_EXECUTE, True, False, CONTROLLER_ID),
                 [(CIF0_SAMPLE_RATE, fixed64(80000000, RADIX_RATE))], False)
        f = exchange(build_cam(ACTION_EXECUTE, True, False, CONTROLLER_ID),
                     [(CIF0_GAIN, struct.pack(">I", 30 << RADIX_GAIN))], False)
        assert abs(f["sample_rate"] - 80000000) < 1, f
        assert abs(f["gain"] - 30) < 0.1, f
        print("rate+gain ack ok:", round(f["sample_rate"]), round(f["gain"]))

        # 4. Dry run must not change state: tune away, expect no new apply.
        f = exchange(build_cam(ACTION_DRY_RUN, True, False, CONTROLLER_ID),
                     [(CIF0_RF_REF_FREQ, fixed64(5180000000, RADIX_FREQ))], False)
        assert abs(f["rf_freq"] - 2412000000) < 1, "dry run changed state"
        print("dry run ok: state held at", round(f["rf_freq"]))

        time.sleep(0.3)
        with open(log) as fh:
            applied = [ln.strip() for ln in fh if ln.strip()]
        assert "rf_freq_hz 2412000000" in applied, applied
        assert "sample_rate_hz 80000000" in applied, applied
        assert "gain_db 30" in applied, applied
        assert not any("5180000000" in a for a in applied), "dry run applied"
        print("apply hook ok:", applied)
        print("\nPASS: mock-hook command/acknowledge subset verified (not hardware)")
        return 0
    finally:
        proc.terminate()
        proc.wait()


if __name__ == "__main__":
    sys.exit(main())

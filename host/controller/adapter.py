#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Controller for the vendored spectrum viewer (host/spectrum). The viewer speaks
# newline-delimited JSON to a controller child process; this maps that protocol
# to the board's on-device control daemon (iq-c2d) over UDP, so the viewer's
# Start/Stop/Tune drive the board with no SSH.
#
# iq-c2d (udp :4992) verbs used here: DISCOVER, PROFILE, STATUS, START, STOP,
# TUNE. It streams VITA 49 back to the command's own source address and resolves
# the host MAC itself, so the host never announces its address. VITA 49.2
# (vita49-control, :4991) remains the control path for other SoapySDR/VITA 49.2
# applications; this adapter is specific to the vendored viewer's protocol.
#
# Tuning snaps to the nearest supported Wi-Fi channel centre (the stock Wi-Fi
# stack tunes on the channel grid, not arbitrary Hz). The 40 MHz capture window
# is wider than the channel spacing, so any requested frequency lands inside it.
# See docs/control-plane.md and the limitations in the top README.
#
# Run the viewer with:
#   w1700k-spectrum --controller "python3 host/controller/adapter.py" ...
#
# Env (defaults for the direct 2.5 GbE link):
#   BOARD=10.77.0.10  C2_PORT=4992  STREAM_PORT=4991  FMT=vrt16
import json, os, socket, sys, threading

B       = os.environ.get("BOARD", "10.77.0.10")
C2_PORT = int(os.environ.get("C2_PORT", "4992"))
PORT    = os.environ.get("STREAM_PORT", "4991")
FMT     = os.environ.get("FMT", "vrt16")

lo_hz = None
sample_rate = None
band = None
rf_width = None
running = False
lock = threading.Lock()


def c2d(request, timeout=8.0):
    """Send one request to the board's iq-c2d daemon and return its reply.

    The source address the kernel picks on the route to the board is the host
    side of the link, which is what iq-c2d streams back to, so no bind is needed.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.settimeout(timeout)
    try:
        s.sendto(request.encode(), (B, C2_PORT))
        reply, _ = s.recvfrom(4096)
    finally:
        s.close()
    text = reply.decode(errors="replace").strip()
    if not text.startswith("OK"):
        raise RuntimeError(text)
    return text


def kv(text, key):
    for tok in text.split():
        if tok.startswith(key + "="):
            return tok[len(key) + 1:]
    return None


def snap(hz):
    """Nearest supported channel centre: 2.4 GHz channels 1..13 (5 MHz steps),
    5 GHz channels 36/40/44/48 (U-NII-1, 20 MHz steps)."""
    hz = int(hz)
    if hz < 3_700_000_000:
        ch = max(0, min(12, round((hz - 2_412_000_000) / 5_000_000)))
        return 2_412_000_000 + ch * 5_000_000
    idx = max(0, min(3, round((hz - 5_180_000_000) / 20_000_000)))
    return 5_180_000_000 + idx * 20_000_000


def send(obj):
    sys.stdout.write(json.dumps(obj) + "\n")
    sys.stdout.flush()


def state():
    return {
        "busy": False,
        "radio": {"loaded": lo_hz is not None, "band": band, "rf_band": band, "lo_hz": lo_hz,
                  "sample_rate": sample_rate, "capture_rate": sample_rate,
                  "bands_ready": [0, 1], "synth_locked": [None, None, None]},
        "stream": {"running": running, "packing": "raw" if running else None,
                   "sample_rate": sample_rate, "center_hz": lo_hz},
        "rf": {"lo_hz": lo_hz, "agc": None, "lna": None, "lpf": None,
               "filter_mhz": rf_width / 1e6 if rf_width else None,
               "lo_range_hz": [2412000000, 5240000000]},
        "error": None,
    }


def emit_state():
    send({"event": "state", "state": state()})


def log(level, text):
    send({"event": "log", "level": level, "text": text})


def refresh_profile():
    global lo_hz, sample_rate, band, rf_width
    reply = c2d("PROFILE")
    lo_hz = int(kv(reply, "freq"))
    sample_rate = int(kv(reply, "rate"))
    band = int(kv(reply, "band"))
    rf_width = 20_000_000


def do_start():
    global running
    # No freq/rate passed: iq-c2d uses the active profile, so it cannot disagree.
    c2d(f"START port={PORT} fmt={FMT}")
    running = True


def do_stop():
    global running
    c2d("STOP")
    running = False


def do_tune(hz):
    global running
    target = snap(hz)
    if lo_hz == target:
        return
    was = running
    if was:
        do_stop()
    # The tune stops capture, reloads the radio and waits ~25 s to settle.
    c2d(f"TUNE freq={target}", timeout=45.0)
    refresh_profile()
    log("info", f"tuned to {lo_hz / 1e6:g} MHz (nearest channel to {int(hz) / 1e6:g})")
    if was:
        do_start()


def handle(req):
    op = req.get("op")
    rid = req.get("id")
    try:
        with lock:
            if op == "status":
                refresh_profile()
            elif op in ("start", "configure"):
                do_start()
            elif op == "stop":
                do_stop()
            elif op == "tune":
                do_tune(req.get("hz", lo_hz))
            elif op in ("gain", "filter", "trim"):
                raise ValueError(f"{op} is not implemented on the MT7981")
            else:
                raise ValueError(f"unknown op {op!r}")
        send({"id": rid, "ok": True, "state": state()})
    except Exception as exc:
        send({"id": rid, "ok": False, "error": str(exc)})
    emit_state()


def main():
    try:
        refresh_profile()     # PROFILE both confirms the board and seeds state
    except Exception as exc:
        log("error", f"board control unreachable at {B}:{C2_PORT}: {exc}")
    emit_state()
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
            if not isinstance(req, dict) or not isinstance(req.get("id"), int):
                raise ValueError("request needs an integer id")
        except Exception as exc:
            send({"event": "log", "level": "error", "text": f"bad request: {exc}"})
            continue
        handle(req)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Verifies the VITA 49 emitter (host/stream/iq-vita49.c) against a parser that
# mirrors what a VITA 49 receiver decodes: the IF-Context fields (frequency,
# sample rate, data format) and a byte-exact sample round-trip, for Ci8 and Ci16.
# Hardware-free. Exit 0 on pass.
import os, socket, struct, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 54991


def build(tmp):
    exe = os.path.join(tmp, "iq-vita49")
    subprocess.run(["cc", "-O2", "-o", exe, os.path.join(ROOT, "host/stream/iq-vita49.c")],
                   check=True)
    return exe


def collect(dur=2.0):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", PORT))
    s.settimeout(dur)
    ctx, payload, counts = None, bytearray(), []
    try:
        while True:
            d, _ = s.recvfrom(65536)
            w0 = struct.unpack(">I", d[:4])[0]
            t, sz, cnt = (w0 >> 28) & 0xF, (w0 & 0xFFFF) * 4, (w0 >> 16) & 0xF
            if t == 0x4:
                cif0 = struct.unpack(">I", d[8:12])[0]; off = 12
                rf = sr = fmt = None
                if cif0 & (1 << 27):
                    rf = struct.unpack(">q", d[off:off+8])[0] / (1 << 20); off += 8
                if cif0 & (1 << 21):
                    sr = struct.unpack(">q", d[off:off+8])[0] / (1 << 20); off += 8
                if cif0 & (1 << 15):
                    fw = struct.unpack(">I", d[off:off+4])[0]
                    fmt = ((fw >> 29) & 3, (fw >> 24) & 0x1F, (fw & 0x1F) + 1)
                ctx = (rf, sr, fmt)
            elif t == 0x1:
                payload += d[8:sz]; counts.append(cnt)
    except socket.timeout:
        pass
    return ctx, bytes(payload), counts


def case(exe, bits, make_payload):
    with tempfile.NamedTemporaryFile(suffix=".iq", delete=False) as f:
        src = make_payload(); f.write(src); path = f.name
    fmt = "ci16" if bits == 16 else "ci8"
    import threading
    out = {}
    th = threading.Thread(target=lambda: out.update(zip(("ctx", "pay", "cnt"), collect())))
    th.start(); time.sleep(0.4)
    subprocess.run([exe, "--format", fmt, "--freq", "2437000000", "--rate", "40000000",
                    "--dst", f"127.0.0.1:{PORT}", path], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    th.join(); os.unlink(path)
    ctx, pay, cnt = out["ctx"], out["pay"], out["cnt"]
    real_cpx, dif, nbits = ctx[2]
    assert ctx[0] == 2437000000.0, f"{fmt}: rf {ctx[0]}"
    assert ctx[1] == 40000000.0, f"{fmt}: rate {ctx[1]}"
    assert (real_cpx, dif, nbits) == (1, 0, bits), f"{fmt}: format {ctx[2]}"
    if bits == 8:
        assert pay == src[:len(pay)], f"{fmt}: payload mismatch"
    else:  # VRT int16 is big-endian on the wire; source is host (little) endian
        got = struct.unpack(f">{len(pay)//2}h", pay)
        want = struct.unpack(f"<{len(pay)//2}h", src[:len(pay)])
        assert got == want, f"{fmt}: payload mismatch"
    assert all(((cnt[i-1]+1) & 0xF) == cnt[i] for i in range(1, len(cnt))), f"{fmt}: seq gap"
    print(f"ok  {fmt}: context {ctx}, {len(pay)} payload bytes, {len(cnt)} packets, seq contiguous")


def main():
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        case(exe, 8, lambda: bytes(b for n in range(4096)
                                   for b in ((n*3) & 0xff, (255-n*2) & 0xff)))
        case(exe, 16, lambda: b"".join(struct.pack("<hh", n*5-4000, 3000-n*3)
                                       for n in range(2048)))
    print("PASS")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.CalledProcessError) as e:
        print("FAIL:", e); sys.exit(1)

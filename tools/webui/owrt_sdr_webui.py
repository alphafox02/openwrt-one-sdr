#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 CEMAXECUTER LLC.
"""
owrt_sdr_webui — live (gapped) spectrum viewer for the OpenWrt One SDR.

Holds one persistent SSH pipe to the router, where tools/live/iqloop.sh loops:
native EXT56 capture -> retire -> read the 64 KiB native window (0x00400000).
Each frame is decoded with the MT7981 native layout (two 32 KiB banks; each
16-byte beat = low 8 bytes from bank0 + high 8 bytes from bank1; path0 = words
0,2, path1 = words 1,3; I = high16 & 0x3fff, Q = low16 & 0x3fff, signed 14-bit)
and served to a local page as spectrum + waterfall + constellation.

This is GAPPED: each frame is one ~8192-sample-per-path snapshot (~200 us at an
assumed 40 MSps), a few frames per second. It is a live spectrum analyser, not a
gap-free stream. SSH is a bring-up transport only; the real transport is a UDP
sender in the W1700K-SDR wire format.

Usage:
    python3 owrt_sdr_webui.py --host 192.168.68.70 --key ~/.ssh/owrt_one
    # open http://127.0.0.1:8088/

Requires numpy. The page is self-contained (no CDN), works offline.
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
MAGIC = b"IQ01"
WINDOW = 65536
FULL_SCALE = 8192.0


def ssh_cmd(cfg, remote):
    return ["ssh", "-T", "-i", cfg.key,
            "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
            "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=8",
            "-o", "ServerAliveInterval=5",
            f"{cfg.user}@{cfg.host}", remote]


def s14(a):
    a = a & 0x3FFF
    return (a ^ 0x2000).astype(np.int32) - 0x2000


def decode(window):
    """64 KiB native window -> (path0, path1) complex arrays, 8192 samples each."""
    w = np.frombuffer(window, dtype="<u4")
    beats = np.hstack([w[:8192].reshape(-1, 2), w[8192:].reshape(-1, 2)])
    iq = lambda x: (s14(x >> 16) + 1j * s14(x & 0xFFFF)) / FULL_SCALE
    p0 = np.column_stack([iq(beats[:, 0]), iq(beats[:, 2])]).ravel()
    p1 = np.column_stack([iq(beats[:, 1]), iq(beats[:, 3])]).ravel()
    return p0, p1


def psd_dbfs(x, n):
    """Averaged Hann periodogram, DC centred, scaled so a full-scale tone ~ 0 dBFS."""
    win = np.hanning(n)
    segs = x[: len(x) // n * n].reshape(-1, n)
    spec = np.fft.fftshift(np.fft.fft(segs * win, axis=1), axes=1) / win.sum()
    return 10 * np.log10(np.mean(np.abs(spec) ** 2, axis=0) + 1e-15)


class Stream(threading.Thread):
    """Owns the SSH pipe to the router loop and the latest decoded frame."""

    def __init__(self, cfg):
        super().__init__(daemon=True)
        self.cfg = cfg
        self.lock = threading.Lock()
        self.latest = None
        self.seq = 0
        self.fps = 0.0
        self.status = "starting"
        self.proc = None
        self.stopping = False
        self.centre = 0.0
        self.aps = []

    def ensure_iqd(self):
        """Make sure the iqd daemon is running on the router (start it if not)."""
        r = subprocess.run(ssh_cmd(self.cfg,
            "pgrep -f /root/iqd >/dev/null || "
            "(/root/iqd --port %d >/tmp/iqd.log 2>&1 & sleep 1; cat /tmp/iqd.log)"
            % self.cfg.iqd_port), capture_output=True, text=True, timeout=20)
        if "not found" in (r.stdout + r.stderr):
            raise RuntimeError("iqd not on router (/root/iqd); build+push it first")

    def iw_dev(self):
        return subprocess.run(ssh_cmd(self.cfg, "iw dev"), capture_output=True,
                              text=True, timeout=15).stdout

    def centre_hz(self):
        for mhz in re.findall(r"channel \d+ \((\d+) MHz\)", self.iw_dev()):
            if int(mhz) < 3000:
                return int(mhz) * 1e6
        return 0.0

    def scan_aps(self):
        """One-shot neighbour scan on the 2.4 GHz vif (before streaming starts)."""
        vif = None
        for name, mhz in re.findall(r"Interface (\S+)[\s\S]*?channel \d+ \((\d+) MHz\)",
                                    self.iw_dev()):
            if int(mhz) < 3000:
                vif = name
                break
        if not vif:
            return []
        out = subprocess.run(ssh_cmd(self.cfg, f"iw dev {vif} scan ap-force"),
                             capture_output=True, text=True, timeout=40).stdout
        aps, cur = [], {}
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("BSS "):
                if cur.get("freq"):
                    aps.append(cur)
                cur = {}
            elif line.startswith("freq:"):
                cur["freq"] = float(line.split()[1]) * 1e6
            elif line.startswith("signal:"):
                cur["dbm"] = float(line.split()[1])
            elif line.startswith("SSID:"):
                cur["ssid"] = line[5:].strip() or "(hidden)"
        if cur.get("freq"):
            aps.append(cur)
        best = {}                                   # strongest AP per channel
        for ap in aps:
            ap.setdefault("ssid", "(hidden)")
            if ap["freq"] < 3e9 and "dbm" in ap:
                if ap["freq"] not in best or ap["dbm"] > best[ap["freq"]]["dbm"]:
                    best[ap["freq"]] = ap
        return sorted(best.values(), key=lambda a: a["freq"])

    def refresh_centre(self):
        while not self.stopping:
            time.sleep(10)
            try:
                c = self.centre_hz()
                if c:
                    self.centre = c
            except Exception:
                pass

    def run(self):
        self.centre = self.cfg.centre
        if not self.cfg.centre:
            threading.Thread(target=self.refresh_centre, daemon=True).start()
        while not self.stopping:
            sock = None
            try:
                self.status = "starting iqd on router"
                self.ensure_iqd()
                self.centre = self.cfg.centre or self.centre_hz()
                if self.cfg.scan and not self.aps:
                    self.status = "scanning for neighbour APs"
                    self.aps = self.scan_aps()
                self.status = f"connecting to iqd {self.cfg.iqd_host}:{self.cfg.iqd_port}"
                sock = socket.create_connection((self.cfg.iqd_host, self.cfg.iqd_port), timeout=10)
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                self.status = "streaming"
                self.pump(sock)
            except Exception as e:
                self.status = f"error: {e}"
            if sock:
                sock.close()
            if not self.stopping:
                time.sleep(2)

    def pump(self, sock):
        buf = bytearray()
        need = len(MAGIC) + WINDOW
        last = time.time()
        while not self.stopping:
            chunk = sock.recv(1 << 16)
            if not chunk:
                self.status = "iqd closed; reconnecting"
                return
            buf += chunk
            while len(buf) >= need:
                at = buf.find(MAGIC)
                if at < 0:
                    del buf[: len(buf) - 3]
                    break
                if at:
                    del buf[:at]          # resync on the frame magic
                    continue
                if len(buf) < need:
                    break
                window = bytes(buf[len(MAGIC):need])
                del buf[:need]
                now = time.time()
                dt, last = now - last, now
                self.fps = 0.8 * self.fps + 0.2 * (1.0 / dt if dt > 0 else 0.0)
                self.process(window, self.centre)

    def process(self, window, centre):
        p0, p1 = decode(window)
        x0, x1 = p0 - p0.mean(), p1 - p1.mean()
        e0, e1 = np.vdot(x0, x0).real, np.vdot(x1, x1).real
        xcorr = float(abs(np.vdot(x0, x1)) / np.sqrt(e0 * e1)) if e0 and e1 else 0.0
        n = self.cfg.fft
        frame = {
            "psd0": np.round(psd_dbfs(p0, n), 1).tolist(),
            "psd1": np.round(psd_dbfs(p1, n), 1).tolist(),
            "rms0_dbfs": round(float(10 * np.log10(np.mean(np.abs(x0) ** 2) + 1e-15)), 1),
            "rms1_dbfs": round(float(10 * np.log10(np.mean(np.abs(x1) ** 2) + 1e-15)), 1),
            "xcorr": round(xcorr, 3),
            "iq": np.round(np.column_stack([p0[:512].real, p0[:512].imag]), 4).ravel().tolist(),
            "centre_hz": centre,
            "fs_hz": self.cfg.fs,
            "fft": n,
        }
        with self.lock:
            self.seq += 1
            frame["seq"] = self.seq
            self.latest = frame

    def snapshot(self):
        with self.lock:
            return self.latest, self.seq

    def stop(self):
        self.stopping = True
        if self.proc:
            self.proc.kill()


class Handler(BaseHTTPRequestHandler):
    stream = None

    def log_message(self, *a):
        pass

    def send(self, code, body, ctype):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urlparse(self.path).path
        if path in ("/", "/index.html"):
            with open(os.path.join(HERE, "index.html"), "rb") as f:
                self.send(200, f.read(), "text/html; charset=utf-8")
        elif path == "/api/latest":
            frame, _ = self.stream.snapshot()
            body = dict(frame or {}, status=self.stream.status, fps=round(self.stream.fps, 2),
                        aps=self.stream.aps)
            self.send(200, json.dumps(body).encode(), "application/json")
        else:
            self.send(404, b'{"error":"not found"}', "application/json")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.68.70", help="router SSH host (control)")
    ap.add_argument("--user", default="root")
    ap.add_argument("--key", default=os.path.expanduser("~/.ssh/owrt_one"))
    ap.add_argument("--iqd-host", default=None, help="iqd data host (default: --host)")
    ap.add_argument("--iqd-port", type=int, default=7700, help="iqd TCP port")
    ap.add_argument("--port", type=int, default=8088, help="local web port")
    ap.add_argument("--fft", type=int, default=512, help="FFT size (power of two)")
    ap.add_argument("--fs", type=float, default=40e6,
                    help="sample rate per path, Hz (ASSUMED until a tone test confirms it)")
    ap.add_argument("--centre", type=float, default=0.0,
                    help="centre frequency, Hz (default: read the 2.4 GHz channel from iw)")
    ap.add_argument("--no-scan", dest="scan", action="store_false",
                    help="skip the one-time neighbour-AP scan used for spectrum markers")
    cfg = ap.parse_args()
    if not cfg.iqd_host:
        cfg.iqd_host = cfg.host

    stream = Stream(cfg)
    stream.start()
    Handler.stream = stream
    srv = ThreadingHTTPServer(("127.0.0.1", cfg.port), Handler)
    print(f"owrt-sdr-webui: http://127.0.0.1:{cfg.port}/  (router {cfg.user}@{cfg.host})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        stream.stop()
        srv.shutdown()


if __name__ == "__main__":
    sys.exit(main())

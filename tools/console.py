#!/usr/bin/env python3
import serial, sys, time
port="/dev/ttyACM0"; baud=115200
s=serial.Serial(port, baud, timeout=0.4, exclusive=True)
try: s.setDTR(True); s.setRTS(True)
except Exception: pass
def drain(t=1.5):
    end=time.time()+t; buf=b""
    while time.time()<end:
        chunk=s.read(4096)
        if chunk: buf+=chunk; end=time.time()+0.5
    return buf.decode(errors="replace")
# wake up / get a prompt
s.write(b"\r"); time.sleep(0.4); banner=drain(1.2)
if "login:" in banner.lower():
    s.write(b"root\r"); time.sleep(0.8); banner+=drain(1.2)
    if "password:" in banner.lower():
        s.write(b"\r"); time.sleep(0.8); banner+=drain(1.2)
def run(cmd, t=3.0):
    s.reset_input_buffer()
    s.write(("echo __B__; "+cmd+"; echo __E__\r").encode())
    time.sleep(0.3); out=drain(t)
    # extract between markers if present
    if "__B__" in out and "__E__" in out:
        out=out.split("__B__")[-1].split("__E__")[0]
    return out.strip("\r\n")
if __name__=="__main__":
    print("[banner tail]", banner[-200:].replace("\r",""))
    for c in sys.argv[1:]:
        print(f"\n$ {c}\n{run(c)}")
    s.close()

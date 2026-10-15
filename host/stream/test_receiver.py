#!/usr/bin/env python3
"""Exercise the actual UDP parser with a clean stream, a gap and bad metadata."""
import signal, socket, struct, subprocess, sys, time
receiver = sys.argv[1] if len(sys.argv)>1 else '/tmp/iq-udp-recv'
for case, positions, bad_offset, expected in [
    ('contiguous', [0,1,2,3], False, 0),
    ('missing', [0,2,3], False, 1),
    ('invalid_offset', [0,1,2,3], True, 1),
]:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.bind(('127.0.0.1',0)); port=s.getsockname()[1]
    p=subprocess.Popen([receiver,str(port),'4'],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    assert 'listening' in p.stderr.readline()
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as s:
        for i in positions:
            seq,half=divmod(i,2)
            offset=1 if bad_offset and i==1 else half*512
            h=struct.pack('<4sIIHHIHHII',b'IQD1',123,seq,offset,512,40000000,1,0,0xe0200000+(seq%64)*8192,0)
            s.sendto(h+bytes([1,255])*512,('127.0.0.1',port))
    time.sleep(.1)
    if p.poll() is None:p.send_signal(signal.SIGINT)
    out,err=p.communicate(timeout=3)
    assert p.returncode==expected,(case,p.returncode,out,err)
    print(case+': '+out.strip())

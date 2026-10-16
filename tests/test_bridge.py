#!/usr/bin/env python3
"""Drive both bridges on localhost; check malformed input and SHM boundaries.
Usage: python3 tests/test_bridge.py /path/to/vita49-shm /path/to/vita49-shm-flat
"""
import mmap
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time


def context(rate=40000000,bits=16):
    return struct.pack('>IIIQQII',0x40000009,1,(1<<27)|(1<<21)|(1<<15),
        2437000000<<20,rate<<20,(1<<29)|((bits-1)<<6)|(bits-1),0)

def wait_for(fn):
    until=time.monotonic()+3
    while time.monotonic()<until:
        if fn(): return
        time.sleep(.02)
    raise AssertionError('bridge did not publish expected state')

for binary in sys.argv[1:]:
    name=f'owrt-test-{os.getpid()}'
    path=Path('/dev/shm')/name
    sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]; sock.close()
    sock=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
    env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0')
    proc=subprocess.Popen([binary,'--port',str(port),'--stream','/'+name,'--ring','8'],
                          stderr=subprocess.PIPE,env=env)
    mem=None
    try:
        assert b'UDP' in proc.stderr.readline(), 'bridge failed to start'
        with path.open('r+b') as f: mem=mmap.mmap(f.fileno(),0)
        def u64(off): return struct.unpack_from('=Q',mem,off)[0]
        def send(pkt): sock.sendto(pkt,('127.0.0.1',port))
        # The underflow reproducer, missing context bodies, and truncated UDP.
        for pkt in (struct.pack('>II',0x10000001,1),struct.pack('>II',0x40000002,1),
                    struct.pack('>II',0x10000302,1)+b'\0'*3072): send(pkt)
        send(context())
        def samples(seq):
            return struct.pack('>II',0x10000000|((seq&15)<<16)|258,1)+struct.pack('>hh',400,-800)*256
        for i in range(8): send(samples(i))
        wait_for(lambda:u64(144)>=1)
        assert proc.poll() is None
        assert u64(312)>=3, 'bad datagrams not counted'
        run=u64(136)
        offset=u64(32)
        assert struct.unpack_from('=hh',mem,offset)==(100,-200)
        for i in range(9,17): send(samples(i)) # missing packet 8
        wait_for(lambda:u64(136)>run and u64(144)>=1)
        run=u64(136)
        send(context(80000000))
        for i in range(17,25): send(samples(i))
        wait_for(lambda:u64(136)>run and u64(144)>=1)
        print('PASS',binary,'malformed/truncated packets, samples, gap and rate-change boundaries')
    finally:
        if mem is not None: mem.close()
        sock.close(); proc.terminate(); proc.wait(timeout=3)
        errors=proc.stderr.read().decode(); proc.stderr.close()
        path.unlink(missing_ok=True)
        assert 'Sanitizer' not in errors,errors

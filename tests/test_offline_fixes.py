#!/usr/bin/env python3
"""Offline regressions: localhost control, mocked SSH and boot preparation.
Build host/controller/vita49-control first. No router access.
"""
import importlib.util
from pathlib import Path
import os
import socket
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
def module(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
c = module('control_tests', 'host/controller/test/test_control.py')
adapter = module('adapter', 'host/controller/adapter.py')

class Control(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.hook = Path(self.tmp.name)/'apply'
        self.hook.write_text('#!/bin/sh\n[ "$1" = --check ] && exit 0\nexit 1\n')
        self.hook.chmod(0o755)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.addCleanup(self.sock.close)
        reserve = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        reserve.bind(('127.0.0.1',0)); port=reserve.getsockname()[1]; reserve.close()
        self.proc = subprocess.Popen([str(ROOT/'host/controller/vita49-control'),
            '--addr','127.0.0.1','--port',str(port),'--apply',str(self.hook)], stderr=subprocess.PIPE)
        self.addCleanup(self.stop)
        self.assertIn(b'listening',self.proc.stderr.readline())
        self.sock.connect(('127.0.0.1',port)); self.sock.settimeout(.5)

    def stop(self):
        self.proc.terminate(); self.proc.wait(timeout=3); self.proc.stderr.close()

    def command(self, action=0, fields=()):
        return c.build_command(1,c.build_cam(action,True,action==0,c.CONTROLLER_ID),c.CONTROLLER_ID,fields)

    def exchange(self, pkt):
        self.sock.send(pkt); return self.sock.recv(65536)

    def rejected(self, pkt):
        with self.assertRaises(socket.timeout): self.exchange(pkt)
        self.assertIsNone(self.proc.poll())

    def test_preflight_failure_keeps_state(self):
        self.hook.write_text('#!/bin/sh\nexit 1\n')
        self.rejected(self.command(2,[(c.CIF0_SAMPLE_RATE,c.fixed64(80000000,20))]))
        _,cam,fields=c.parse_ack(self.exchange(self.command()),True)
        self.assertFalse(cam&(1<<10)); self.assertEqual(fields['sample_rate'],40000000)
        self.assertNotIn('gain',fields); self.assertNotIn('bandwidth',fields)

    def test_execution_failure_invalidates_state(self):
        self.rejected(self.command(2,[(c.CIF0_SAMPLE_RATE,c.fixed64(80000000,20))]))
        self.rejected(self.command())

    def test_uuid_roundtrip(self):
        ids=struct.pack('>8I',1,2,3,4,5,6,7,8)
        cam=(1<<31)|(1<<30)|(1<<29)|(1<<28)|(1<<18)
        body=struct.pack('>III',c.STREAM_ID,cam,77)+ids+struct.pack('>I',0)
        pkt=struct.pack('>I',0x60000000+(len(body)+4)//4)+body
        ack=self.exchange(pkt)
        self.assertEqual(ack[16:48],ids)
        self.assertEqual(struct.unpack_from('>I',ack,48)[0],0x08200000)
        self.rejected(pkt[:24])

    def test_no_partial_batch_or_unknown_fields(self):
        log=Path(self.tmp.name)/'calls'
        self.hook.write_text(f'#!/bin/sh\necho called >> {log}\n')
        self.rejected(self.command(2,[(c.CIF0_RF_REF_FREQ,c.fixed64(2412000000,20)),
                                     (c.CIF0_SAMPLE_RATE,c.fixed64(80000000,20))]))
        self.rejected(self.command(2,[(1<<28,c.fixed64(0,20))]))
        self.assertFalse(log.exists())

    def test_fractional_gain(self):
        log=Path(self.tmp.name)/'calls'
        self.hook.write_text(f'#!/bin/sh\n[ "$1" = --check ] && exit 0\necho "$1 $2" > {log}\n')
        ack=self.exchange(self.command(2,[(c.CIF0_GAIN,struct.pack('>I',int(3.25*128)))]))
        _,cam,fields=c.parse_ack(ack,False)
        self.assertTrue(cam&(1<<10)); self.assertEqual(fields['gain'],3.25)
        self.assertEqual(log.read_text().strip(),'gain_db 3.25')

class Readback(unittest.TestCase):
    def test_live_state_and_cross_band_ack(self):
        with tempfile.TemporaryDirectory() as td:
            d=Path(td); state=d/'state'; hook=d/'hook'
            state.write_text('2437000000 40000000 20000000\n')
            hook.write_text(f'''#!/bin/sh
case "$1" in
--state) cat '{state}';;
--check) exit 0;;
rf_freq_hz) echo "$2 20000000 20000000" > '{state}';;
*) exit 1;;
esac
''')
            hook.chmod(0o755)
            with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
                sock.bind(('127.0.0.1',0)); port=sock.getsockname()[1]
            proc=subprocess.Popen([str(ROOT/'host/controller/vita49-control'),
                '--addr','127.0.0.1','--port',str(port),'--apply',str(hook),'--readback'],stderr=subprocess.PIPE)
            try:
                self.assertIn(b'listening',proc.stderr.readline())
                with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as sock:
                    sock.connect(('127.0.0.1',port)); sock.settimeout(2)
                    def exchange(action,fields=()):
                        sock.send(c.build_command(1,c.build_cam(action,True,action==0,c.CONTROLLER_ID),c.CONTROLLER_ID,fields))
                        return c.parse_ack(sock.recv(65536), action==0)[2]
                    exchange(2,[(c.CIF0_RF_REF_FREQ,c.fixed64(5180000000,20))])
                    actual=exchange(0)
                    self.assertEqual(actual['rf_freq'],5180000000)
                    self.assertEqual(actual['sample_rate'],20000000)
                    self.assertEqual(actual['bandwidth'],20000000)
                    state.write_text('5220000000 40000000 20000000\n')
                    self.assertEqual(exchange(0)['rf_freq'],5220000000)
                    state.write_text('invalid\n')
                    sock.settimeout(.5)
                    with self.assertRaises(socket.timeout): exchange(0)
            finally:
                proc.terminate(); proc.wait(timeout=3); proc.stderr.close()

class Adapter(unittest.TestCase):
    # The viewer adapter drives the board over the iq-c2d network daemon (no SSH).
    def setUp(self): adapter.running=False; adapter.lo_hz=2437000000; adapter.band=0
    def test_failed_start_reports_no_success(self):
        with mock.patch.object(adapter,'c2d',side_effect=RuntimeError('ERR start failed')):
            with self.assertRaises(RuntimeError): adapter.do_start()
        self.assertFalse(adapter.running)
    def test_snap_to_nearest_channel(self):
        self.assertEqual(adapter.snap(2450000000), 2452000000)   # 2.4 GHz, 5 MHz grid
        self.assertEqual(adapter.snap(2400000000), 2412000000)   # clamp low
        self.assertEqual(adapter.snap(5195000000), 5200000000)   # 5 GHz U-NII-1
        self.assertEqual(adapter.snap(5300000000), 5240000000)   # clamp high
    def test_retune_snaps_settles_and_restarts(self):
        reqs=[]; adapter.running=True
        def c2d(req, timeout=8.0):
            reqs.append(req)
            if req.startswith('PROFILE'): return 'OK freq=5180000000 rate=40000000 band=1 bw=0'
            if req.startswith('STOP'):    return 'OK stopped'
            if req.startswith('TUNE'):    return 'OK tuned freq=5180000000 capture=stopped'
            if req.startswith('START'):   return 'OK streaming to 10.77.0.11:4991 fmt=vrt16 freq=5180000000 rate=40'
            return 'OK'
        with mock.patch.object(adapter,'c2d',side_effect=c2d), mock.patch.object(adapter,'log'):
            adapter.do_tune(5178000000)   # off-grid: must snap to 5180000000
        self.assertTrue(reqs[0].startswith('STOP'))
        self.assertIn('TUNE freq=5180000000', reqs[1])
        self.assertIn('PROFILE', reqs[2])
        self.assertTrue(reqs[-1].startswith('START'))
        self.assertEqual(adapter.state()['radio']['rf_band'], 1)
        self.assertTrue(adapter.running)

class Boot(unittest.TestCase):
    def test_preparation_latches_and_falls_back(self):
        with tempfile.TemporaryDirectory() as td:
            d=Path(td)
            for name,value in [('gold','GOLD'),('stock','STOCK'),('sdr','SDR'),('driver','DRIVER')]:
                (d/name).write_text(value)
            script=f'''
. '{ROOT}/init/owrt-sdr-prepare.init'
logger() {{ :; }}
sync() {{ :; }}
GOLD_FW='{d}/gold'; GOLD_DRIVER='{d}/stock'
SDR_FW='{d}/sdr'; SDR_DRIVER='{d}/driver'
ACTIVE_FW='{d}/active'; ACTIVE_DRIVER='{d}/module'
LATCH='{d}/pending'; SELECTED='{d}/selected'
boot || exit 10
[ "$(cat "$ACTIVE_FW")" = SDR ] && [ -f "$LATCH" ] && [ -f "$SELECTED" ] || exit 11
boot || exit 12
[ "$(cat "$ACTIVE_FW")" = GOLD ] && [ "$(cat "$ACTIVE_DRIVER")" = STOCK ] || exit 13
[ ! -e "$SELECTED" ] && [ -f "$LATCH" ] || exit 14
rearm || exit 15
boot || exit 16
[ "$(cat "$ACTIVE_FW")" = SDR ] || exit 17
'''
            subprocess.run(['sh','-c',script],check=True)

    def test_start_checks_daemon_and_configuration(self):
        with tempfile.TemporaryDirectory() as td:
            d=Path(td)
            (d/'fw').write_text('firmware'); (d/'driver').write_text('module')
            (d/'icap').write_text(f"D='{d}'\n")
            (d/'iq_udp').touch()
            daemon=d/'start-stop-daemon'
            daemon.write_text('#!/bin/sh\nexit 0\n'); daemon.chmod(0o755)
            script=f'''
. '{ROOT}/init/owrt-sdr.init'
logger() {{ :; }}
sync() {{ :; }}
sleep() {{ :; }}
wifi() {{ :; }}
uci() {{ echo 6; }}
PATH="{d}:$PATH"
SDR_IP=''; SDR_FW='{d}/fw'; ACTIVE_FW='{d}/fw'
DRIVER='{d}/driver'; ACTIVE_DRIVER='{d}/driver'
READY='{d}/ready'; LATCH='{d}/pending'; PIDFILE='{d}/pid'; SELECTED='{d}/selected'
ICAP_HELPER='{d}/icap'; CONTROL=/bin/true; APPLY=/bin/true
touch "$SELECTED"
if start; then exit 10; fi
[ -f "$LATCH" ] && [ ! -f "$READY" ] || exit 11
# Supply a live mock PID, then verify the success path.
echo $$ > "$PIDFILE"
start || exit 12
[ -f "$READY" ] && [ -f "$LATCH" ] || exit 13

'''
            subprocess.run(['sh','-c',script],check=True)

    def test_stop_preserves_latch_on_drain_failure(self):
        with tempfile.TemporaryDirectory() as td:
            d=Path(td)
            mock_daemon=d/'start-stop-daemon'
            mock_daemon.write_text('#!/bin/sh\nexit 0\n')
            mock_daemon.chmod(0o755)
            script=f'''
. '{ROOT}/init/owrt-sdr.init'
READY='{d}/ready'; LATCH='{d}/pending'; PIDFILE='{d}/pid'
touch "$READY" "$LATCH"
STOP_CAPTURE=/bin/false
if stop; then exit 10; fi
[ -e "$LATCH" ] && [ -e "$READY" ] || exit 11
STOP_CAPTURE=/bin/true
PATH="{d}:$PATH"
sync() {{ :; }}
stop || exit 12
[ ! -e "$LATCH" ] && [ ! -e "$READY" ] || exit 13
'''
            subprocess.run(['sh','-c',script],check=True)

class Radio(unittest.TestCase):
    def test_unchanged_tune_and_rate_do_not_stop_or_reload(self):
        with tempfile.TemporaryDirectory() as td:
            d=Path(td)
            (d/'uci').write_text('''#!/bin/sh
case "$*" in
*radio0.disabled) echo 1;;
*radio1.disabled) echo 0;;
*radio1.sdr_sample_rate) echo 40000000;;
*radio1.channel) echo 36;;
*radio1.htmode) echo HE20;;
*) exit 99;;
esac
''')
            (d/'iw').write_text('#!/bin/sh\necho "channel 36 (5180 MHz), width: 20 MHz, center1: 5180 MHz"\n')
            for f in d.iterdir(): f.chmod(0o755)
            env=dict(os.environ,PATH=f"{d}:"+os.environ['PATH'])
            helper=str(ROOT/'tools/live/iq-radio.sh')
            for cmd in [('tune','5180000000'),('rate','40000000')]:
                subprocess.run([helper,*cmd],env=env,check=True,capture_output=True)

class Apply(unittest.TestCase):
    def test_only_exact_rf_centres_validate(self):
        hook=ROOT/'host/controller/vita49-apply.sh'
        env = dict(os.environ, IQ_RADIO_HELPER=str(ROOT/'tools/live/iq-radio.sh'))
        for value in ('2412000000','2437000000','2472000000','5180000000'):
            self.assertEqual(subprocess.run(['sh',str(hook),'--check','rf_freq_hz',value],env=env).returncode,0)
        for field,value in [('sample_rate_hz','80000000'),('gain_db','3.25'),
                            ('rf_freq_hz','5190000000'),('rf_freq_hz','2437500000'),
                            ('rf_freq_hz','nan'),('rf_freq_hz','-1')]:
            self.assertNotEqual(subprocess.run(['sh',str(hook),'--check',field,value],env=env,stderr=subprocess.DEVNULL).returncode,0)

if __name__=='__main__': unittest.main(verbosity=2)

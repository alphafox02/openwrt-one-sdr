#!/usr/bin/env python3
"""Re-run the cached OpenWrt compile/link commands; avoid a full SDK rebuild."""
from pathlib import Path
import os, subprocess, sys
root = Path(__file__).resolve().parents[2]
base = root / 'build/openwrt/build_dir/target-aarch64_cortex-a53_musl/linux-mediatek_filogic'
src = base / 'mt76-2024.10.28~c246fa54/mt7915'
env = dict(os.environ)
env['STAGING_DIR'] = str(root / 'build/openwrt/staging_dir')
env['PATH'] = str(root / 'build/openwrt/staging_dir/toolchain-aarch64_cortex-a53_gcc-13.3.0_musl/bin') + ':' + env['PATH']
# Structure changes affect every object, including mac/main/testmode.
names = sys.argv[1:] or [Path(s).stem for s in (src/'mt7915e.mod').read_text().split()]
for name in [*names, 'mt7915e', 'mt7915e.ko']:
    cmdfile = src / ('.' + name + ('.cmd' if name.endswith('.ko') else '.o.cmd'))
    cmd = cmdfile.read_text().splitlines()[0].split(' := ', 1)[1]
    subprocess.run(cmd, shell=True, cwd=base/'linux-6.6.63', env=env, check=True)
print(src / 'mt7915e.ko')

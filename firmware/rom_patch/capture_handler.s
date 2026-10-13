	.text
	.globl _start
! openwrt-one-sdr: in-firmware ICAP capture arm (capture_setup.h sequence, tight).
! Interposed on an existing runtime hook (TO[1]: FROM 0x00829d3a -> orig 0x0090122c).
! Runs in live WM context so the DFE->writer datapath takes (host pokes were too
! slow: the capture completed in the ms gaps before the DFE mux was set). Rate-
! limited: only re-arms when ICAP0 CR0.CAPTURE is clear. Preserves r0-r9, then
! tail-jumps to the original handler 0x0090122c. Window result lands at 0x00464000.
! Bases: ICAP 0x80023000  route 0x88009000  DFE 0x830ad000  WFCOMM 0x830a0000  BN 0x83080000
_start:
	addi  $sp, $sp, -40
	swi   $r0, [$sp+0]
	swi   $r1, [$sp+4]
	swi   $r2, [$sp+8]
	swi   $r3, [$sp+12]
	swi   $r4, [$sp+16]
	swi   $r5, [$sp+20]
	swi   $r6, [$sp+24]
	swi   $r7, [$sp+28]
	swi   $r8, [$sp+32]
	swi   $r9, [$sp+36]
	! --- host-triggered one-shot gate: arm only when CR11 scratch == MAGIC ---
	! CR11 (+0xbc, SYSRAM_OFFSET) resets to 0 at boot and is never touched by the
	! engine or firmware (icap.h) => handler is DORMANT at boot (no capture armed
	! before RX exists => no AXI/bus-lock hazard). The host pokes CR11=0xa5a50001
	! AFTER monitor RX is live; the next hook pass arms once and consumes the flag.
	sethi $r1, #0x80023		! ICAP base
	lwi   $r0, [$r1+#0xbc]		! CR11 = arm flag
	sethi $r6, #0xa5a50
	ori   $r6, $r6, #0x001		! MAGIC 0xa5a50001
	bne   $r0, $r6, Ldone		! not armed -> tail to original handler
	movi  $r0, #0
	swi   $r0, [$r1+#0xbc]		! consume flag (one-shot)
	! --- base registers ---
	sethi $r2, #0x88009		! route
	sethi $r3, #0x830ad		! DFE
	sethi $r4, #0x830a0		! WFCOMM/CAPTRIG/CFG
	sethi $r5, #0x83080		! BN_CLK
	! --- capture clear (capture_setup.h step 1a) ---
	movi  $r0, #0
	swi   $r0, [$r4+#0x1004]		! CAPTRIG_CFG = 0
	swi   $r0, [$r3+#0x448]		! DFE MUX = 0
	swi   $r0, [$r3+#0x44c]		! DFE MUX2 = 0
	lwi   $r0, [$r4+#0x3008]
	sethi $r6, #0x200		! bit21
	or    $r0, $r0, $r6
	swi   $r0, [$r4+#0x3008]		! WF_COMM +0x3008 |= [21]
	lwi   $r0, [$r4+#0x3008]
	sethi $r6, #0x400		! bit22
	or    $r0, $r0, $r6
	swi   $r0, [$r4+#0x3008]		! WF_COMM +0x3008 |= [22]
	! --- ICAP0 setup + window route (step 1) ---
	movi  $r0, #0
	swi   $r0, [$r1+#0x90]		! CR0 = 0 (SW_RST_B low)
	movi  $r0, #0x400
	swi   $r0, [$r1+#0x90]		! CR0 = 0x400 (SW_RST_B high / IDLE)
	movi  $r0, #0
	swi   $r0, [$r1+#0xb8]		! CR10 = 0 (no packer)
	sethi $r0, #0xff000		! 0xff000000
	swi   $r0, [$r2+#4]		! route +0x04 = 0xff000000 (window banks)
	movi  $r0, #0
	swi   $r0, [$r2+#0xc]		! route +0x0c = 0 (owner ICAP0)
	sethi $r0, #0x2000		! 0x02000000
	swi   $r0, [$r2+#0x24]		! route +0x24 = 0x02000000 (upper half)
	movi  $r0, #0x400
	swi   $r0, [$r2+#0x28]		! route +0x28 = 0x400 (ICAP0 write path)
	movi  $r0, #0x401
	swi   $r0, [$r1+#0x90]		! CR0 = 0x401 (EN, SRC_SEL=DFE)
	movi  $r0, #0
	swi   $r0, [$r1+#0xb0]		! CR8 = 0 (band 0)
	swi   $r0, [$r1+#0x98]		! CR2 = 0 (ADDR0_START)
	movi  $r0, #0xffc
	swi   $r0, [$r1+#0x9c]		! CR3 = 0xffc (ADDR0_END, 1024 beats)
	! --- CAPTURE (step 2) ---
	movi  $r0, #0x403
	swi   $r0, [$r1+#0x90]		! CR0 = 0x403 (EN | CAPTURE)
	! --- BN_CLK |= CAPTURE bit25 (step 3) ---
	lwi   $r0, [$r5+#4]
	sethi $r6, #0x2000		! 0x02000000
	or    $r0, $r0, $r6
	swi   $r0, [$r5+#4]		! BN_CLK0 |= bit25
	lwi   $r0, [$r5+#8]
	or    $r0, $r0, $r6
	swi   $r0, [$r5+#8]		! BN_CLK1 |= bit25
	! --- DFE configure (step 4) ---
	movi  $r0, #2
	swi   $r0, [$r3+#0x460]		! DFE CTRL = EN (bit1), FREEZE clear
	movi  $r0, #0
	swi   $r0, [$r4+#0x3008]		! WF_COMM +0x3008 = 0
	movi  $r0, #0x10
	swi   $r0, [$r4+#0x1004]		! CAPTRIG_CFG = 0x10
	movi  $r0, #0x11
	swi   $r0, [$r3+#0x440]		! DFE MODE = 0x11 (domain 0 activation)
	sethi $r0, #0x3020		! 0x03020100
	ori   $r0, $r0, #0x100
	swi   $r0, [$r3+#0x468]		! DFE LANE = 0x03020100
	! --- DFE mux: node stock, band 0 (step 5) ---
	sethi $r0, #0x230		! 0x00230014 (SEL0=0x14 LIVE tap, RATE0=3<<16, ACT0=bit21)
	ori   $r0, $r0, #0x14
	swi   $r0, [$r3+#0x448]		! DFE MUX = 0x230014
	! --- capture trigger (step 6) ---
	sethi $r0, #0x4090		! 0x04090100
	ori   $r0, $r0, #0x100
	swi   $r0, [$r4+#0x1000]		! CAPTRIG = 0x04090100
Ldone:
	lwi   $r0, [$sp+0]
	lwi   $r1, [$sp+4]
	lwi   $r2, [$sp+8]
	lwi   $r3, [$sp+12]
	lwi   $r4, [$sp+16]
	lwi   $r5, [$sp+20]
	lwi   $r6, [$sp+24]
	lwi   $r7, [$sp+28]
	lwi   $r8, [$sp+32]
	lwi   $r9, [$sp+36]
	addi  $sp, $sp, 40
	sethi $r15, #0x901		! original hooked handler 0x0090122c
	ori   $r15, $r15, #0x22c
	jr    $r15

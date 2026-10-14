/* SPDX-License-Identifier: MIT
 * MT7981 bounded native-window DMA exporter.
 * Descriptor/header recipe derived from Luigi C.'s BirriaSDR raw_dma.S (MIT).
 * MT7981 ROM reclaimer and accounting derived from the device ROM.
 * Func 3 of EXT56 exports 8 x 8192-byte packets from RETIRED capture memory.
 * Func6 follows the dedicated 512KiB capture pool continuously.
 * Abort on overrun or ambiguous timing; preserve the first fault diagnostics.
 */
.section .text
.global command
.macro load32 reg,val
 sethi \reg, (\val >> 12)
 ori \reg, \reg, (\val & 0xfff)
.endm
command:
 push25 $r8, 0
 beqz $r0, command_null
 lwi $r7, [$r0+12]
 lwi $r6, [$r7+0x40]
 beqc $r6, 3, custom
 beqc $r6, 4, custom2
 beqc $r6, 5, custom
 beqc $r6, 6, custom
 j command_resume
custom:
 lwi $r0, [$r7+0x44]      /* func6 requested packet count; zero defaults to128 */
 jal producer
 pop25 $r8, 0
/* Func 4: gap-free validation. Arm ICAP ring mode (CR0=0x24f3) so the capture
 * runs continuously into the cached 0x00400000 window, let it accumulate, then
 * write back the whole D-cache (0xe00452b6) so the live samples reach DRAM, and
 * export the window by DMA (func-3 producer) WITHOUT retiring. Firing this twice
 * while the ring runs should yield different, live IQ if the writeback works. */
custom2:
 load32 $r4, 0x80023090
 movi $r0, 0x24f3
 swi $r0, [$r4]            /* CR0 = ring start */
 load32 $r1, 0x30000       /* ~accumulate delay */
spin4:
 addi $r1, $r1, -1
 bnez $r1, spin4
 /* VA-range D-cache writeback over [0x400000, 0x420000): sync dirty capture
  * lines to DRAM so the TX0 DMA sees live samples. 4096 x 32-byte lines. */
 load32 $r0, 0x400000
 load32 $r2, 0x1000
wb4:
 cctl $r0, L1D_VA_WB
 addi $r0, $r0, 0x20
 addi $r2, $r2, -1
 bnez $r2, wb4
 dsb
 jal producer             /* DMA-export 0x400000 (now flushed to DRAM) */
 pop25 $r8, 0
producer:
 push25 $r14, 48
 swi $r0, [$sp+44]
 swi $r6, [$sp+16]
 load32 $r14, 0xe009b800
 movi $r0, 1
 swi $r0, [$r14]
 movi $r0, 0
 swi $r0, [$r14+4]
 swi $r0, [$sp+32]
 swi $r0, [$sp+36]
 swi $r0, [$sp+24]
 addi $r2, $r14, 96
 movi $r3, 16
clear_diagnostics:
 swi.bi $r0, [$r2], 4
 addi $r3, $r3, -1
 bnez $r3, clear_diagnostics
 swi $r0, [$r14+64]       /* separate host-owned cache line */
 addi $r0, $r14, 64
 cctl $r0, L1D_VA_WB
 dsb
 swi $gp, [$r14+16]
 /* Allocate a persistent header; on timeout it remains allocated. */
 movi $r0, 80
 jal packet_alloc
 swi $r0, [$sp]
 beqz $r0, alloc_failed
 lwi $r10, [$r0+12]
 swi $r10, [$r14+20]
 mfsr $r0, $psw
 swi $r0, [$sp+4]
 setgie.d
 dsb
 load32 $r6, 0x54000300
 lwi $r7, [$r6]
 lwi $r8, [$r6+8]
 lwi $r0, [$r6+12]
 bne $r0, $r8, busy
 lwi $r0, [$r6+4]
 movi $r1, 4
 bne $r0, $r1, busy
 movi $r0, 0
 movi $r1, 0
 load32 $r2, 0x0081b5b2
 jral5 $r2
 load32 $r0, 0x37d2c
 add $r11, $gp, $r0
 lwi $r0, [$r11]
 bne $r0, $r7, busy
 lwi $r0, [$r11+0x34]
 swi $r0, [$sp+8]
 load32 $r0, 0x89020068
 lwi $r0, [$r0]
 swi $r0, [$sp+12]
 movi $r0, 0
 movi $r1, 11
 mov55 $r2, $r10
clear:
 swi.bi $r0, [$r2], 4
 addi $r1, $r1, -1
 bnez $r1, clear
 load32 $r0, 0x3800202c
 swi $r0, [$r10]
 load32 $r0, 0x38002014
 swi $r0, [$r10+24]
 movi $r0, 0xed
 swi $r0, [$r10+28]
 movi $r0, 0xf1
 swi $r0, [$r10+32]
 load32 $r9, 0x00400000
 /* Func5 exports the first 64KiB of the dedicated ICAP pool.
  * Only use with --pool firmware, after capture has stopped. */
 lwi $r0, [$sp+16]        /* native command selector */
 beqc $r0, 5, pool_source
 bnec $r0, 6, window_source
pool_source:
 load32 $r9, 0xe0200000
window_source:
 movi $r12, 0
 movi $r2, 8
 swi $r2, [$sp+40]
 bnec $r0, 6, publish
 lwi $r2, [$sp+44]
 bnez $r2, have_count
 movi $r2, 128
have_count:
 swi $r2, [$sp+40]
 movi $r2, 0
 swi $r2, [$sp+20]
 swi $r2, [$sp+24]
 /* Datapath already primed by native_capture + retire. Dedicated pool only. */
 load32 $r4, 0x88009028
 movi $r2, 1
 swi $r2, [$r4]
 load32 $r4, 0x80023090
 movi $r2, 0
 swi $r2, [$r4]
 movi $r2, 0x400
 swi $r2, [$r4]
 movi $r2, 0
 swi $r2, [$r4+8]
 load32 $r2, 0x1fffc
 swi $r2, [$r4+12]
 movi $r2, 0x2401
 swi $r2, [$r4]
 load32 $r2, 0x89020068
 lwi $r2, [$r2]
 swi $r2, [$sp+12]
 swi $r2, [$sp+28]
 movi $r2, 0x2403
 swi $r2, [$r4]
publish:
 lwi $r0, [$sp+16]
 bnec $r0, 6, source_ready
 load32 $r1, 1000000
wait_source:
 jal progress
 lwi $r2, [$sp+32]
 bnez $r2, all_consumed
 slti $r2, $r0, 2048
 beqz $r2, source_ready
 addi $r1, $r1, -1
 bnez $r1, wait_source
 movi $r2, 6
 swi $r2, [$sp+32]
 j all_consumed
source_ready:
 load32 $r0, 0x89020068
 lwi $r0, [$r0]
 swi $r0, [$sp+44]       /* request count already moved to sp+40 */
 swi $r12, [$r10+36]
 swi $r9, [$r10+40]
 slli $r3, $r8, 4
 add $r3, $r7, $r3
 swi $r10, [$r3]
 swi $r9, [$r3+8]
 movi $r0, 0
 swi $r0, [$r3+12]
 /* First descriptor: header + samples; second: four-byte trailer.
  * Never read beyond the native capture window for the hardware trailer. */
 load32 $r0, 0x002c2000
 swi $r0, [$r3+4]
 addi $r8, $r8, 1
 andi $r8, $r8, 3
 slli $r3, $r8, 4
 add $r3, $r7, $r3
 swi $r10, [$r3]
 movi $r0, 0
 swi $r0, [$r3+8]
 swi $r0, [$r3+12]
 load32 $r0, 0x40040000
 swi $r0, [$r3+4]
 dsb
 addi $r8, $r8, 1
 andi $r8, $r8, 3
 swi $r8, [$r6+8]
 load32 $r1, 1000000
wait_dma:
 lwi $r0, [$sp+16]
 bnec $r0, 6, check_dma
 jal progress
check_dma:
 lwi $r0, [$r6+12]
 bne $r0, $r8, dma_wait_more
 /* Index equality alone does not prove descriptor writeback is visible. */
 dsb
 addi $r2, $r8, -1
 andi $r2, $r2, 3
 slli $r2, $r2, 4
 add $r2, $r7, $r2
 cctl $r2, L1D_VA_INVAL
 dsb
 lwi $r0, [$r2+4]
 bgez $r0, ddone_lag
 addi $r2, $r8, -2
 andi $r2, $r2, 3
 slli $r2, $r2, 4
 add $r2, $r7, $r2
 cctl $r2, L1D_VA_INVAL
 dsb
 lwi $r0, [$r2+4]
 bltz $r0, consumed
 ddone_lag:
 lwi $r0, [$r14+152]
 addi $r0, $r0, 1
 swi $r0, [$r14+152]
 dma_wait_more:
 addi $r1, $r1, -1
 bnez $r1, wait_dma
 movi $r0, 4
 j timeout
consumed:
 load32 $r2, 0x89020068
 lwi $r2, [$r2]
 lwi $r3, [$sp+44]
 sub $r2, $r2, $r3
 bltz $r2, dma_time_done
 lwi $r3, [$r14+96]
 slt $r3, $r3, $r2
 beqz $r3, dma_time_done
 swi $r2, [$r14+96]
dma_time_done:
 addi $r12, $r12, 1
 swi $r12, [$r14+4]
 addi $r9, $r9, 8192
 load32 $r0, 0xe0280000
 bne $r9, $r0, no_wrap
 load32 $r9, 0xe0200000
no_wrap:
 lwi $r0, [$sp+32]
 bnez $r0, all_consumed
 lwi $r0, [$sp+40]
 bne $r12, $r0, publish
all_consumed:
 lwi $r0, [$sp+16]
 bnec $r0, 6, stop_done
 load32 $r1, 0x80023090
 movi $r0, 0x400
 swi $r0, [$r1]
stop_done:
 /* All descriptors consumed; restore ROM's TX accounting. */
 mov55 $r1, $r7
 movi $r2, 4
 load32 $r0, 0xc0000000
 movi $r3, 0
restore:
 swi $r0, [$r1+4]
 swi $r3, [$r1+8]
 addi $r1, $r1, 16
 addi $r2, $r2, -1
 bnez $r2, restore
 swi $r8, [$r11+0x2c]
 lwi $r0, [$sp+8]
 slli $r1, $r12, 1
 add $r0, $r0, $r1
 swi $r0, [$r11+0x34]
 swi $r0, [$r11+0x38]
 load32 $r1, 0x89020068
 lwi $r0, [$r1]
 lwi $r2, [$sp+12]
 sub $r0, $r0, $r2
 swi $r0, [$r14+8]
 lwi $r0, [$sp+24]
 swi $r0, [$r14+24]
 lwi $r0, [$sp+36]
 swi $r0, [$r14+28]
 lwi $r0, [$sp+32]
 bnez $r0, finish
 movi $r0, 2
 j finish
busy:
 movi $r0, 3
finish:
 swi $r0, [$r14]
 cctl $r14, L1D_VA_WB
 addi $r0, $r14, 32
 cctl $r0, L1D_VA_WB
 addi $r0, $r14, 96
 cctl $r0, L1D_VA_WB
 addi $r0, $r14, 128
 cctl $r0, L1D_VA_WB
 dsb
 lwi $r0, [$sp+4]
 mtsr $r0, $psw
 isb
 lwi $r0, [$sp]
 movi $r1, 1
 jal packet_free
 movi $r0, 0
 pop25 $r14, 48
timeout:
 /* Stop acquisition but keep any DMA-owned header allocated. A driver reset
  * is required after status4; never pretend an in-flight descriptor is free. */
 lwi $r2, [$sp+32]
 bnez $r2, timeout_recorded
 movi $r3, 4
 jal progress_fault
timeout_recorded:
 load32 $r2, 0x80023090
 movi $r0, 0x400
 swi $r0, [$r2]
 movi $r0, 4
 swi $r0, [$r14]
 lwi $r0, [$sp+24]
 swi $r0, [$r14+24]
 lwi $r0, [$sp+36]
 swi $r0, [$r14+28]
 cctl $r14, L1D_VA_WB
 addi $r0, $r14, 32
 cctl $r0, L1D_VA_WB
 addi $r0, $r14, 96
 cctl $r0, L1D_VA_WB
 addi $r0, $r14, 128
 cctl $r0, L1D_VA_WB
 dsb
 lwi $r0, [$sp+4]
 mtsr $r0, $psw
 isb
 movi $r0, 0
 pop25 $r14, 48
alloc_failed:
 movi $r0, 7
 swi $r0, [$r14]
 movi $r0, 0
 pop25 $r14, 48

/* r0=unread words; preserve timeout r1 and all callee-saved registers.
 * Flag faults, but always drain an already submitted descriptor first. */
progress:
 /* Preserve the first fault while draining an in-flight DMA packet. */
 lwi $r2, [$sp+32]
 bnez $r2, progress_fault_return
 addi $r2, $r14, 64
 cctl $r2, L1D_VA_INVAL
 dsb
 lwi $r2, [$r14+64]
 beqz $r2, not_stopped
 movi $r3, 9
 j progress_fault
not_stopped:
 load32 $r2, 0x89020068
 lwi $r2, [$r2]
 lwi $r3, [$sp+28]
 swi $r2, [$sp+28]
 sub $r3, $r2, $r3
 swi $r3, [$r14+36]
 /* Permit only the previously observed small negative counter jitter. */
 bgez $r3, poll_positive
 addi $r3, $r3, 32
 bgez $r3, poll_ok
 j poll_bad
poll_positive:
 lwi $r0, [$r14+100]
 slt $r0, $r0, $r3
 beqz $r0, poll_check
 swi $r3, [$r14+100]
poll_check:
 slti $r3, $r3, 500
 bnez $r3, poll_ok
poll_bad:
 swi $r2, [$r14+40]
 lwi $r3, [$r14+36]
 swi $r3, [$r14+44]
 movi $r3, 8
 j progress_fault
poll_ok:
 load32 $r2, 0x800230b4
 lwi $r2, [$r2]
 lwi $r3, [$sp+20]
 swi $r3, [$r14+120]
 swi $r2, [$r14+124]
 swi $r2, [$sp+20]
 sub $r2, $r2, $r3
 load32 $r3, 0x1ffff
 and $r2, $r2, $r3
 swi $r2, [$r14+128]
 lwi $r3, [$r14+104]
 slt $r3, $r3, $r2
 beqz $r3, delta_peak_done
 swi $r2, [$r14+104]
delta_peak_done:
 lwi $r0, [$sp+24]
 add $r0, $r0, $r2
 swi $r0, [$sp+24]
 slli $r2, $r12, 11
 sub $r0, $r0, $r2
 lwi $r3, [$sp+36]
 slt $r2, $r3, $r0
 beqz $r2, peak_done
 swi $r0, [$sp+36]
peak_done:
 load32 $r3, 0x1f000
 slt $r2, $r0, $r3
 bnez $r2, progress_ok
 movi $r3, 5
progress_fault:
 swi $r3, [$sp+32]
 swi $r3, [$r14+108]
 lwi $r0, [$sp+24]
 slli $r2, $r12, 11
 sub $r0, $r0, $r2
 swi $r0, [$r14+112]
 swi $r12, [$r14+116]
 lwi $r2, [$r14+36]
 swi $r2, [$r14+132]
 lwi $r2, [$r6+12]
 swi $r2, [$r14+136]
 swi $r8, [$r14+140]
 addi $r2, $r8, -2
 andi $r2, $r2, 3
 slli $r2, $r2, 4
 add $r2, $r7, $r2
 lwi $r2, [$r2+4]
 swi $r2, [$r14+144]
 addi $r2, $r8, -1
 andi $r2, $r2, 3
 slli $r2, $r2, 4
 add $r2, $r7, $r2
 lwi $r2, [$r2+4]
 swi $r2, [$r14+148]
progress_ok:
 ret5 $lp
progress_fault_return:
 lwi $r0, [$sp+24]
 slli $r2, $r12, 11
 sub $r0, $r0, $r2
 ret5 $lp

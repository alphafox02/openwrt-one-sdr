/* Diagnostic veneers for gold MT7981 WM e11d45ca... .
 * Preserve original behavior. Mailbox in zero padding at e009b800.
 * Linked at e009b000. Periodic callback exports gp. Command hook traces the actual ICAP dispatcher.
 */
.section .text
.global periodic, command
.macro load32 reg,val
 sethi \reg, (\val >> 12)
 ori \reg, \reg, (\val & 0xfff)
.endm
periodic:
 addi $sp, $sp, -32
 swi $r0, [$sp]
 swi $r1, [$sp+4]
 swi $r2, [$sp+8]
 swi $r3, [$sp+12]
 load32 $r0, 0xe009b800
 lwi $r1, [$r0]
 addi $r1, $r1, 1
 swi $r1, [$r0]
 swi $gp, [$r0+4]
 lwi $r0, [$sp]
 lwi $r1, [$sp+4]
 lwi $r2, [$sp+8]
 lwi $r3, [$sp+12]
 addi $sp, $sp, 32
 push25 $r6, 0
 jal stock_status
 j periodic_resume
command:
 addi $sp, $sp, -16
 swi $r0, [$sp]
 swi $r1, [$sp+4]
 load32 $r0, 0xe009b808
 lwi $r1, [$r0]
 addi $r1, $r1, 1
 swi $r1, [$r0]
 lwi $r0, [$sp]
 lwi $r1, [$sp+4]
 addi $sp, $sp, 16
 push25 $r8, 0
 beqz $r0, command_null
 j command_resume

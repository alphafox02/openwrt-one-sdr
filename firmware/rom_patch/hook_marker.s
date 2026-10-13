	.text
	.globl _start
_start:
	addi  $sp, $sp, -8
	swi   $r2, [$sp+0]
	sethi $r15, #0x900
	ori   $r15, $r15, #0x204     ! 0x900204 (unused FROM/TO slot in patch RAM)
	sethi $r2, #0xc0dec
	ori   $r2, $r2, #0xafe
	swi   $r2, [$r15+0]
	lwi   $r2, [$sp+0]
	addi  $sp, $sp, 8
	sethi $r15, #0x901
	ori   $r15, $r15, #0x92
	jr    $r15

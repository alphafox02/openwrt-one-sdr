	.text
	.globl _start
_start:
	addi  $sp, $sp, -8
	swi   $r2, [$sp+0]
	sethi $r15, #0x900
	ori   $r15, $r15, #0x204
	sethi $r2, #0x800
	ori   $r2, $r2, #0x0           ! r2 = 0x00800000 (ROM, known = 0x6d030048)
	lwi   $r2, [$r2+0]
	swi   $r2, [$r15+0]
	lwi   $r2, [$sp+0]
	addi  $sp, $sp, 8
	sethi $r15, #0x901
	ori   $r15, $r15, #0x92
	jr    $r15

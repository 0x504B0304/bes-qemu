.syntax unified
.cpu cortex-m33
.thumb
.section .text
.space 16
.global _start
.thumb_func
_start:
    cpsid i
    ldr r4, =12000000
    ldr r5, =0x20004000
    ldr r6, =0x40002004
    movs r0, #0
    str r0, [r5]
1:
    ldr r0, [r5]
    adds r0, #1
    str r0, [r5]
    ldr r1, [r6]
    eors r1, r0
    subs r4, #1
    bne 1b
    /* ARM semihosting SYS_EXIT_EXTENDED, success. */
    movs r0, #0x20
    adr r1, exit_block
    bkpt #0xab
    b .
.align 2
exit_block:
    .word 0x20026, 0

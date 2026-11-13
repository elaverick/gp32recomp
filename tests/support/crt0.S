@ Start-up code for the test programs: a GXB header, then straight into C.
    .syntax unified
    .arm
    .section .init, "ax"
    .global _start
_start:
    b       start
    .word   __text_start, __text_end, __data_start, __data_end, __bss_start, __bss_end
    .word   0x44450011, 0x44450011
    .word   0x01234567, 0x12345678, 0x23456789, 0x34567890, 0x45678901, 0x56789012
start:
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    mov     r2, #0
1:  cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b
    ldr     r0, =main
    mov     lr, pc
    bx      r0
    cmp     r0, #0
    ldreq   r1, =0x20026
    movne   r1, #1
    mov     r0, #0x18
    swi     0x123456
2:  b       2b

    @ void sys_write0(const char *s)
    .global sys_write0
sys_write0:
    mov     r1, r0
    mov     r0, #4
    swi     0x123456
    bx      lr

    @ unsigned swi_call(unsigned n_in_r12_unused, ...) helpers for firmware tests
    .global bios_tick_ptr
bios_tick_ptr:
    mov     r0, #6
    swi     0x0b
    bx      lr
    .global bios_install_irq
bios_install_irq:
    swi     0x09
    bx      lr
    .global irq_enable
irq_enable:
    mrs     r0, cpsr
    bic     r0, r0, #0x80
    msr     cpsr_c, r0
    bx      lr

@ Corner cases a compiler never produces, checked against values worked out
@ by hand from the ARM architecture manual.  r4 collects one bit per failure.
    .syntax unified
    .arm
    .text
    .global edge_cases
    .macro expect reg, value, bit
    ldr     r12, =\value
    cmp     \reg, r12
    orrne   r4, r4, #(1 << \bit)
    .endm
    .macro flags reg
    mrs     \reg, cpsr
    lsr     \reg, \reg, #28
    .endm
edge_cases:
    push    {r4-r11, lr}
    mov     r4, #0
    mov     r0, #0
    msr     cpsr_f, r0
    @ 0: LSL by 32 through a register: result 0, carry = bit 0
    mov     r0, #1
    mov     r1, #32
    movs    r2, r0, lsl r1
    flags   r3
    expect  r3, 0x6, 0
    @ 1: LSL by 33: result 0, carry clear
    mov     r1, #33
    movs    r2, r0, lsl r1
    flags   r3
    expect  r3, 0x4, 1
    @ 2: LSR #32 (encoded as LSR #0): result 0, carry = bit 31
    mov     r0, #0x80000000
    movs    r2, r0, lsr #32
    flags   r3
    expect  r3, 0x6, 2
    @ 3: ASR by 40 of a negative number: all ones, carry set
    mov     r1, #40
    movs    r2, r0, asr r1
    flags   r3
    expect  r3, 0xA, 3
    expect  r2, 0xFFFFFFFF, 4
    @ 5: RRX shifts the carry in at the top
    mov     r0, #3
    cmp     r0, #0              @ sets carry
    movs    r2, r0, rrx
    flags   r3
    expect  r2, 0x80000001, 5
    expect  r3, 0xA, 6
    @ 7: ROR by 32 through a register leaves the value, carry = bit 31
    mov     r0, #0x80000000
    mov     r1, #32
    movs    r2, r0, ror r1
    flags   r3
    expect  r3, 0xA, 7
    @ 8: shift by zero in a register leaves the carry alone
    mov     r0, #1
    cmp     r0, #0              @ carry set
    mov     r1, #0
    movs    r2, r0, lsl r1
    flags   r3
    expect  r3, 0x2, 8
    @ 9: ADC and SBC carry chain: 0xFFFFFFFF + 0 + C(1) = 0 with carry out
    mvn     r0, #0
    cmp     r0, #0              @ carry set
    adcs    r2, r0, #0
    flags   r3
    expect  r3, 0x6, 9
    @ 10: 0 - 0 - !C(0) = 0xFFFFFFFF, borrow (carry clear), negative
    mov     r0, #0
    cmn     r0, #0              @ carry clear
    sbcs    r2, r0, #0
    flags   r3
    expect  r3, 0x8, 10
    @ 11: signed overflow 0x7FFFFFFF + 1
    mvn     r0, #0x80000000
    adds    r2, r0, #1
    flags   r3
    expect  r3, 0x9, 11
    @ 12: RSC
    mov     r0, #5
    cmp     r0, #0              @ carry set
    rscs    r2, r0, #7
    expect  r2, 2, 12
    @ 13: MSR writes flags only; MRS reads them back
    mov     r0, #0xF0000000
    msr     cpsr_f, r0
    flags   r3
    expect  r3, 0xF, 13
    mov     r0, #0
    msr     cpsr_f, r0
    @ 14: reading pc gives the instruction address plus 8
    adr     r0, 1f
1:  mov     r1, pc
    sub     r1, r1, r0
    expect  r1, 8, 14
    @ 15: STM stores pc as the instruction address plus 12
    sub     sp, sp, #8
    adr     r0, 2f
2:  stmia   sp, {pc}
    ldr     r1, [sp]
    add     sp, sp, #8
    sub     r1, r1, r0
    expect  r1, 12, 15
    @ 16: LDM with the base in the list keeps the loaded value
    adr     r0, 3f
    ldmia   r0!, {r0, r1}
    expect  r0, 0x11111111, 16
    expect  r1, 0x22222222, 17
    b       4f
3:  .word   0x11111111, 0x22222222
4:
    @ 18: unaligned LDR rotates the word
    adr     r0, 3b
    ldr     r1, [r0, #1]
    expect  r1, 0x11111111, 18
    adr     r0, 5f
    ldr     r1, [r0, #1]
    expect  r1, 0x44112233, 19
    b       6f
5:  .word   0x11223344
6:
    @ 20: long multiplies
    mvn     r0, #0
    mov     r1, #2
    umull   r2, r3, r0, r1
    expect  r2, 0xFFFFFFFE, 20
    expect  r3, 1, 21
    smull   r2, r3, r0, r1
    expect  r2, 0xFFFFFFFE, 22
    expect  r3, 0xFFFFFFFF, 23
    mov     r2, #5
    mov     r3, #0
    umlal   r2, r3, r0, r1
    expect  r2, 3, 24
    expect  r3, 2, 25
    @ 26: SWP
    sub     sp, sp, #4
    mov     r0, #0x55
    str     r0, [sp]
    mov     r1, #0xAA
    swp     r2, r1, [sp]
    ldr     r3, [sp]
    add     sp, sp, #4
    expect  r2, 0x55, 26
    expect  r3, 0xAA, 27
    @ 28: conditional execution of every condition after "cmp 1, 2"
    mov     r0, #1
    mov     r5, #0
    cmp     r0, #2
    orreq   r5, r5, #1 << 0
    orrne   r5, r5, #1 << 1
    orrcs   r5, r5, #1 << 2
    orrcc   r5, r5, #1 << 3
    orrmi   r5, r5, #1 << 4
    orrpl   r5, r5, #1 << 5
    orrvs   r5, r5, #1 << 6
    orrvc   r5, r5, #1 << 7
    orrhi   r5, r5, #1 << 8
    orrls   r5, r5, #1 << 9
    orrge   r5, r5, #1 << 10
    orrlt   r5, r5, #1 << 11
    orrgt   r5, r5, #1 << 12
    orrle   r5, r5, #1 << 13
    expect  r5, 0x2A9A, 28
    mov     r0, r4
    pop     {r4-r11, lr}
    bx      lr
    .ltorg

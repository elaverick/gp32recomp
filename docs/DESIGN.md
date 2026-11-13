# Design notes

## The machine

| | |
| --- | --- |
| CPU | Samsung S3C2400 (ARM920T core, ARMv4T), 66 MHz by default |
| RAM | 8 MB at `0x0C000000`; the firmware keeps the top 512 KB |
| Program | loaded at `0x0C000000`, entered at its first word in SVC mode |
| Display | 320x240. The panel is mounted sideways: the frame buffer is 320 columns of 240 pixels, each column bottom to top. 8-bit with a palette at `0x14A00400`, or 16-bit `RRRRRGGGGGBBBBBI` |
| Buttons | port B bits 8-15 (left, down, right, up, L, B, A, R) and port E bits 6-7 (START, SELECT), active low |
| Sound | IIS, normally fed by DMA channel 2 |
| Interrupts | the boot ROM jumps through a table of 32 handlers at `0x0C7AC000`; `SWI 9` installs one |

An `.fxe` is a header (title, author, icon) followed by the program XORed
with a key stored in the header. A `.gxb` is the bare program.

## Translation

`recomp/gp32recomp.cpp` works in three steps.

**Finding code.** A work list starts at the entry point. Each run of
instructions is decoded until an unconditional transfer; branch targets and
call targets go back on the list. Along the way constants loaded into
registers are tracked, which resolves veneers (`ldr pc, =f`, `bx r12`) and
finds addresses the code takes. Jump tables of both common shapes
(`add pc, pc, rX, lsl #2` and `ldr pc, [pc, rX, lsl #2]`) are read using the
bound from the preceding `cmp`. Finally every aligned word in the image that
points back into the image is tried as a possible function, and accepted if
it decodes sensibly. A wrong guess only adds dead code.

**Grouping.** Call targets and guessed entries start C functions. A function
is its entry plus every block reachable without passing another entry.

**Writing C.** Each instruction is a call such as
`arm_dp(c, 0xE0810002u, 0x0C000108u)`: the handler, the instruction word and
the value the instruction would read from `pc`. Control flow is written out
explicitly.

## Running native and interpreted code together

All guest state lives in one `gp32_cpu` structure; nothing is kept in C local
variables. That gives one rule which everything else relies on:

> A translated function may return at any time, as long as `r[15]` holds the
> address of the next instruction to run.

The dispatcher in `runtime/cpu.c` then looks that address up and continues,
natively if a translated block starts there and in the interpreter otherwise.
Consequences:

- A guest call is a C call followed by a check that the callee came back to
  the expected address. If it did not (an interrupt, a `longjmp`, a context
  switch), the caller returns too and the dispatcher takes over. The native
  stack is only ever a shortcut.
- An interrupt is taken at a block boundary by changing `r[15]`, mode and
  `lr` exactly as the processor would, then returning.
- Indirect jumps first try the blocks of the current function, then the
  global table, then the interpreter.
- If the program overwrites translated code, native execution is switched
  off and the interpreter runs what is actually in memory.

## Time

`c->cycles` counts instructions down to the next hardware deadline; each
block subtracts its length. At zero, `gp32_event()` runs whatever is due
(timers, the 1 ms tick, sound, the end of a frame), delivers interrupts and
reloads the counter. Emulated seconds are instructions divided by
`FCLK / cpi`, so the program sees the timer and display behaviour of a real
GP32 no matter how fast the host is. At each frame the runtime sleeps until
real time catches up.

## Firmware calls

The runtime answers these `SWI` numbers (see `runtime/bios.c`):

| SWI | Service |
| --- | --- |
| `0x02`, `0x07`, `0x17` | MMU remap, port reset, volume: accepted, no effect |
| `0x04` | exit |
| `0x05` | run a program image in memory |
| `0x08` | LCD: set mode, get surface, enable, status, info |
| `0x09`, `0x0A` | install / remove an interrupt handler |
| `0x0B` | addresses of firmware data: flip callback, palette, clock info, tick counter |
| `0x0D` | change the CPU clock |
| `0x0F` | application path and user info |
| `0x10` | key port addresses and key mapping |
| `0x13` | firmware timers (the SDK's 1000 Hz callback and 500 Hz scheduler tick) |
| `0x15` | command line (always empty) |
| `0x16` | palette operations |
| `0x123456` | ARM semihosting (console output, heap info, exit) for programs linked with a stock newlib start-up |

Where the firmware hands out the address of one of its own routines (the
surface flip callback, the exit address in `lr` at start-up) the runtime
gives an address in the absent ROM; jumping there runs a native function.

The SDK timer rates were worked out from the SDK libraries: `GpTimerSet`
turns a requested rate into scheduler ticks as `(1000 / rate) / 2`, which
is only right if the scheduler ticks 500 times a second.

## What is missing

- SmartMedia: the NAND interface on the GPIO pins, and the firmware's card
  routines that the SDK file library calls through `SWI 0x0B` function 3.
- SDK sound: `SWI 0x0E` and `SWI 0x14`.
- An unpacking step in the translator for self-extracting `.fxe` files.
- Game controllers, full-screen mode and filtering in the front end.

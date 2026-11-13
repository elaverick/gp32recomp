# gp32recomp

A static recompiler for the Gamepark GP32. It reads a GP32 program (`.fxe` or
`.gxb`), translates its ARM920T machine code to C, and links that against a
runtime that stands in for the console's hardware and firmware. The result is
an ordinary Windows executable: no emulator, no BIOS dump.

```
game.fxe --gp32recomp--> C source + runtime --your C compiler--> game.exe
```

## Status

Working and tested:

- **CPU**: all of ARMv4T, ARM and Thumb, including interworking, banked
  registers and exception return. Checked by test programs whose output must
  match the same C source compiled for the host, run both recompiled and
  through the built-in interpreter (`tests/run.sh`, also run in CI on Linux
  and Windows).
- **Hardware**: LCD (8-bit palettised and 16-bit, with the sideways frame
  buffer and beam position), buttons, the five PWM timers, the RTC tick,
  the interrupt controller, DMA (memory copies and IIS sound streaming),
  clock control and the UART as a debug console.
- **Firmware**: interrupt handler installation, the official SDK's start-up
  calls, screen surfaces and palette, key reading, the millisecond tick
  counter, and the timers that drive the SDK's thread scheduler.
- **Front end**: a Win32 window with sound through waveOut and keyboard
  input.

Three real programs were used as checks: two official-SDK graphics samples
(8-bit and 16-bit) draw the expected picture, and a module player built on
hardware-level sound produces continuous music.

Not done yet:

- **SmartMedia card / files.** Programs that load data with `GpFileOpen` or
  the SMFS library find an empty card slot. This rules out most commercial
  games for now.
- **The official SDK's sound calls** (`GpPcmInit`, `GpPcmPlay`). Programs
  that drive the sound hardware themselves do work.
- **Encrypted `.gxe` files** from retail cards.
- **Packed `.fxe` files** unpack themselves at run time, so there is no code
  to translate ahead of time. They still run, in the interpreter, at reduced
  speed.

## Building the tool

You need CMake 3.15+ and a C++17 compiler (Visual Studio 2019 or later, or
MinGW-w64, or GCC/Clang on Linux and macOS).

```
cmake -B build
cmake --build build --config Release
```

This produces `gp32recomp` (`build\Release\gp32recomp.exe` with Visual
Studio). The runtime sources are embedded in it, so the one file is all you
need.

## Translating a program

```
gp32recomp game.fxe -o game_recomp
cmake -S game_recomp -B game_recomp/build
cmake --build game_recomp/build --config Release
```

The output folder is a self-contained CMake project: the translated code, a
copy of the runtime, and the original image (the program's own data is still
needed at run time). Building it gives `game.exe`.

Keys: arrow keys for the stick, `X` = A, `Z` = B, `A` = L, `S` = R,
`Enter` = START, `Backspace` = SELECT, `Esc` to quit.

Useful options of the built program (`game.exe --help` lists all):

| Option | Effect |
| --- | --- |
| `--scale N` | Window size, in multiples of 320x240 |
| `--cpi X` | CPU cycles charged per instruction (default 2). Lower is a faster GP32 |
| `--interp` | Ignore the translated code and interpret everything |
| `--trace` | Log firmware calls and stray memory accesses |
| `--frames N --dump f.bmp` | Run N frames without a window and save the last one |
| `--wav f.wav` | Record the sound output |

`gp32recomp --help` lists the translator's options, such as `--func ADDR` to
point it at code it did not find by itself.

## How it works

See [docs/DESIGN.md](docs/DESIGN.md). In short:

- The translator follows calls and branches from the entry point, then
  checks every pointer-sized value in the image for more code (callbacks,
  jump tables, interrupt handlers).
- Each instruction becomes one call to a handler in `runtime/gp32_ops.h` with
  the instruction word as a constant. The C compiler folds the decoding away.
  The interpreter calls the same handlers, so there is one definition of
  what every instruction does.
- Anything the translator missed is interpreted at run time, so a missed
  function costs speed, not correctness.
- Time is counted in executed instructions. Timers, the display and sound
  all run off that clock, and the program sleeps at each frame to stay at the
  original speed.

## Legal

This project contains no Gamepark code. Use it with software you have the
right to use. Translated output contains the original program and is subject
to that program's licence.

MIT licensed, see [LICENSE](LICENSE).

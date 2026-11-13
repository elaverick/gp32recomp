# gp32recomp

A static recompiler for the Gamepark GP32. It reads a GP32 program (`.fxe` or
`.gxb`), translates its ARM920T code to C, and links the result against a
runtime that stands in for the console's hardware and firmware, giving a
native Windows executable.

**Status: work in progress.** The translator, the CPU core and the hardware
model are in place and under test; the Windows front end, SmartMedia support
and the documentation are not written yet.

## Layout

- `recomp/` - the `gp32recomp` tool (C++17)
- `runtime/` - the library every translated program links against (C11)
- `tests/` - GP32 test programs and the script that runs them

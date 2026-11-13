#!/bin/sh
# Builds each test program for the GP32, recompiles it, and checks that the
# native result prints exactly what the same C source prints when compiled
# for the host (or a fixed expected file for the hardware tests).  Every
# program is also run through the interpreter.
#
# usage: tests/run.sh [build-dir]        needs clang with the ARM target, lld, cmake
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/build}
T=$B/tests
GEN=""
command -v ninja >/dev/null 2>&1 && GEN="-G Ninja"
CC_ARM="clang --target=armv4t-none-eabi -mcpu=arm920t -mfloat-abi=soft -O2 -ffreestanding -fno-pic -Wall -I$ROOT/tests/support"

cmake -S "$ROOT" -B "$B" $GEN >/dev/null
cmake --build "$B" >/dev/null
mkdir -p "$T"
$CC_ARM -marm -c "$ROOT/tests/support/crt0.S" -o "$T/crt0.o"
$CC_ARM -marm -c "$ROOT/tests/support/libc.c" -o "$T/libc.o"
$CC_ARM -marm -c "$ROOT/tests/cpu/edge.S" -o "$T/edge.o"

fail=0
for t in alu mem flow hash edge irq; do
    src=$ROOT/tests/cpu/t_$t.c
    if [ -f "$ROOT/tests/cpu/t_$t.expected" ]; then
        cp "$ROOT/tests/cpu/t_$t.expected" "$T/$t.expected"
    else
        cc -O1 -I"$ROOT/tests/support" "$src" "$ROOT/tests/support/libc.c" -o "$T/$t.ref"
        "$T/$t.ref" > "$T/$t.expected"
    fi
    for mode in arm thumb; do
        n=$t-$mode
        $CC_ARM -m$mode -c "$src" -o "$T/$n.o"
        ld.lld -T "$ROOT/tests/support/gp32.ld" "$T/crt0.o" "$T/$n.o" "$T/edge.o" "$T/libc.o" -o "$T/$n.elf"
        llvm-objcopy -O binary "$T/$n.elf" "$T/$n.gxb"
        "$B/gp32recomp" "$T/$n.gxb" -o "$T/$n" --runtime "$ROOT/runtime" >/dev/null
        cmake -S "$T/$n" -B "$T/$n/build" $GEN -DGP32_HEADLESS=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$T/$n/build" >"$T/$n.buildlog" 2>&1 || { echo "FAIL  $n (build, see $T/$n.buildlog)"; fail=1; continue; }
        for how in native interp; do
            flag=""
            [ $how = interp ] && flag="--interp"
            if "$T/$n/build/${n%-*}_$mode" --no-throttle --no-audio $flag >"$T/$n.$how.out" 2>&1 && cmp -s "$T/$n.$how.out" "$T/$t.expected"; then
                echo "ok    $n ($how)"
            else
                echo "FAIL  $n ($how)"
                diff "$T/$t.expected" "$T/$n.$how.out" | head -8
                fail=1
            fi
        done
    done
done
exit $fail

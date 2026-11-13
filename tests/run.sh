#!/bin/sh
# Builds each test program for the GP32, recompiles it, builds the result
# natively and compares what it prints with tests/cpu/t_*.expected.  Every
# program is run twice: as recompiled code and through the interpreter.
#
# The expected files for the pure C tests were produced by compiling the same
# source for the host; REGEN=1 does that again.
#
# usage: tests/run.sh [build-dir]     needs cmake, and clang + lld with the ARM target
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
B=${1:-$ROOT/build}
T=$B/tests
GEN=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) ;;                      # let CMake pick Visual Studio
    *) command -v ninja >/dev/null 2>&1 && GEN="-G Ninja" ;;
esac
HEADLESS=${GP32_HEADLESS:-ON}
CC_ARM="clang --target=armv4t-none-eabi -mcpu=arm920t -mfloat-abi=soft -O2 -ffreestanding -fno-pic -Wall -I$ROOT/tests/support"

exe() { # path of a built program, whichever layout the generator used
    for p in "$1/$2" "$1/$2.exe" "$1/Release/$2.exe"; do [ -f "$p" ] && { echo "$p"; return; }; done
    echo "$1/$2"
}

cmake -S "$ROOT" -B "$B" $GEN -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$B" --config Release >/dev/null
RECOMP=$(exe "$B" gp32recomp)
mkdir -p "$T"
$CC_ARM -marm -c "$ROOT/tests/support/crt0.S" -o "$T/crt0.o"
$CC_ARM -marm -c "$ROOT/tests/support/libc.c" -o "$T/libc.o"
$CC_ARM -marm -c "$ROOT/tests/cpu/edge.S" -o "$T/edge.o"

fail=0
for t in alu mem flow hash edge irq; do
    src=$ROOT/tests/cpu/t_$t.c
    expected=$ROOT/tests/cpu/t_$t.expected
    if [ -n "$REGEN" ] && [ $t != edge ] && [ $t != irq ]; then
        cc -O1 -I"$ROOT/tests/support" "$src" "$ROOT/tests/support/libc.c" -o "$T/$t.ref"
        "$T/$t.ref" > "$expected"
    fi
    for mode in arm thumb; do
        n=$t-$mode
        $CC_ARM -m$mode -c "$src" -o "$T/$n.o"
        ld.lld -T "$ROOT/tests/support/gp32.ld" "$T/crt0.o" "$T/$n.o" "$T/edge.o" "$T/libc.o" -o "$T/$n.elf"
        llvm-objcopy -O binary "$T/$n.elf" "$T/$n.gxb"
        "$RECOMP" "$T/$n.gxb" -o "$T/$n" --runtime "$ROOT/runtime" >/dev/null
        cmake -S "$T/$n" -B "$T/$n/build" $GEN -DGP32_HEADLESS=$HEADLESS -DCMAKE_BUILD_TYPE=Release >/dev/null
        cmake --build "$T/$n/build" --config Release >"$T/$n.buildlog" 2>&1 || { echo "FAIL  $n (build)"; tail -20 "$T/$n.buildlog"; fail=1; continue; }
        prog=$(exe "$T/$n/build" "${t}_$mode")
        for how in native interp; do
            flag=""
            [ $how = interp ] && flag="--interp"
            "$prog" --no-throttle --no-audio --frames 100000 $flag 2>&1 | tr -d '\r' >"$T/$n.$how.out" || true
            if cmp -s "$T/$n.$how.out" "$expected"; then
                echo "ok    $n ($how)"
            else
                echo "FAIL  $n ($how)"
                diff "$expected" "$T/$n.$how.out" | head -8
                fail=1
            fi
        done
    done
done
exit $fail

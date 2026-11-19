#!/usr/bin/env bash
# Build a real C program to Udon and run it under the headless VM.
#
# libsm64 is the acceptance program for this backend: 58 translation units, a
# megabyte of IR, function pointers, structs, unions, varargs calls, a heap and
# half a million slots. It is not a fixture; it is a library written for a
# hosted target and never adjusted for this one.
#
# Usage:
#   scripts/build-libsm64.sh <libsm64-src> <driver.c> [extra llvm-udon flags]
#
# Both paths and the toolchain paths are handed to native binaries, so they must
# be in the host's own form (C:/... on Windows), not MSYS form. The defaults are
# this machine's; override with the environment:
#
#   UDON_CLANG      clang that emits 32-bit-pointer IR (llvm-mingw i686)
#   UDON_LLVM_BIN   directory holding llvm-link
#   UDON_HEAP_BYTES heap bytes; libsm64's memory_init wants tens of megabytes
#   UDON_MEMORY_BYTES end of the globals image. libsm64's driver declares a few
#                   megablocks of geometry buffers, so the default is 4 MiB
#                   rather than the backend's 1 MiB.
#
# The driver must define `udon_event_start`, which becomes the `_start` entry.
set -euo pipefail

SCRIPT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(dirname "$SCRIPT")

if [ $# -lt 2 ]; then
  echo "usage: $0 <libsm64-src> <driver.c> [llvm-udon flags]" >&2
  exit 1
fi
SRC=$1
DRIVER=$2
shift 2

CL=${UDON_CLANG:-/i/Downloads/llvm-mingw-20240619-ucrt-x86_64/bin/i686-w64-mingw32-clang.exe}
BIN=${UDON_LLVM_BIN:-/w/projects/llvm-cbe/llvm-project/llvm/build/build64/bin}
HEAP=${UDON_HEAP_BYTES:-67108864}
MEMORY=${UDON_MEMORY_BYTES:-4194304}
OUT="$ROOT/build/libsm64"
IR="$OUT/ir"
mkdir -p "$IR"

# The same flags the library was written for, minus the hosted assumptions.
# -fno-vectorize/-fno-slp-vectorize keep `<16 x i8>` copies out of the IR; the
# writer scalarizes what still appears, but not paying for it is better.
CFLAGS=(-ffreestanding -fno-builtin -fno-strict-aliasing -fno-common
        -O2 -S -emit-llvm -w
        -fno-vectorize -fno-slp-vectorize -fno-unroll-loops
        -DSM64_LIB_EXPORT -DGBI_FLOATS -DVERSION_US -DNO_SEGMENTED_MEMORY
        -I "$SRC/src/decomp/include")

DIRS="src src/decomp src/decomp/engine src/decomp/include/PR src/decomp/game
      src/decomp/pc src/decomp/pc/audio src/decomp/mario src/decomp/tools
      src/decomp/audio"

N=0
for d in $DIRS; do
  for f in "$SRC/$d"/*.c; do
    [ -e "$f" ] || continue
    rel=$(printf '%s' "$f" | sed "s|$SRC/||; s|/|_|g; s|\.c$||")
    "$CL" "${CFLAGS[@]}" -o "$IR/$rel.ll" "$f"
    N=$((N + 1))
  done
done
echo "compiled $N translation units"

# The freestanding libc, and the driver, are compiled with the same force-include
# so neither needs a #include and both see one set of declarations.
"$CL" "${CFLAGS[@]}" -O1 -include "$ROOT/runtime/libc/udon_libc.h" \
  -o "$IR/libc.ll" "$ROOT/runtime/libc/libc.c"
"$CL" "${CFLAGS[@]}" -include "$ROOT/runtime/libc/udon_libc.h" \
  -o "$IR/driver.ll" "$DRIVER"

# One module: the writer's call graph, slot layout and phi lowering are all
# per-module, so the library, its libc and the driver must be linked first.
"$BIN/llvm-link.exe" -o "$OUT/program.bc" "$IR"/*.ll
echo "linked $(stat -c%s "$OUT/program.bc") bytes"

"$ROOT/build/tools/llvm-udon/llvm-udon.exe" "$OUT/program.bc" \
  -o "$OUT/program.uasm" -udon-heap-bytes="$HEAP" \
  -udon-memory-bytes="$MEMORY" "$@"
echo "wrote $(stat -c%s "$OUT/program.uasm") bytes"

dotnet "$ROOT/tools/Udon.VM/bin/Debug/net9.0/udon-vm.dll" run \
  "$OUT/program.uasm" --heap-json "$OUT/program.udonheap.json" --entry _start

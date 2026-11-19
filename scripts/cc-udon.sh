#!/usr/bin/env bash
# Compile C to Udon: clang -> IR -> (link libc) -> llvm-udon.
#
# The backend has no C frontend of its own, and the LLVM tree it lives in is
# built without clang, so the frontend is supplied from outside. Any clang that
# emits 32-bit-pointer IR works; the default is the i686 llvm-mingw build
# because its pointer width matches this target's data layout.
#
# Usage:
#   scripts/cc-udon.sh input.c -o output.uasm [extra llvm-udon flags]
#
# Environment:
#   UDON_CLANG   clang to use (default: the llvm-mingw i686 build)
#   UDON_CFLAGS  extra C flags, appended after the defaults
#   UDON_NO_LIBC set to 1 to link without runtime/libc/libc.c

set -euo pipefail

# Resolve the repo root from this script's own path, not from the caller's cwd,
# so the script works when invoked by relative path, absolute path, or symlink.
SCRIPT=$(realpath "${BASH_SOURCE[0]}")
ROOT=$(dirname "$(dirname "$SCRIPT")")
CLANG=${UDON_CLANG:-/i/Downloads/llvm-mingw-20240619-ucrt-x86_64/bin/i686-w64-mingw32-clang.exe}

# llvm-link and llvm-udon come from the same build as the backend.
LLVM_BIN=${UDON_LLVM_BIN:-/w/projects/llvm-cbe/llvm-project/llvm/build/build64/bin}
# Windows toolchains name their executables .exe, and bash will not add the
# suffix, so pick whichever exists.
exe() {
  if [ -x "$1" ]; then echo "$1"
  elif [ -x "$1.exe" ]; then echo "$1.exe"
  else echo "$1"; fi
}
LLVM_LINK=$(exe "$LLVM_BIN/llvm-link")
LLVM_UDON=$(exe "${UDON_BIN:-$ROOT/build/tools/llvm-udon/llvm-udon}")

if [ ! -x "$CLANG" ]; then
  echo "cc-udon: clang not found: $CLANG" >&2
  echo "cc-udon: set UDON_CLANG to a clang that emits 32-bit-pointer IR" >&2
  exit 1
fi

INPUT=
OUTPUT=
PASSTHRU=()
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUTPUT=$2; shift 2 ;;
    *.c) INPUT=$1; shift ;;
    *) PASSTHRU+=("$1"); shift ;;
  esac
done

if [ -z "$INPUT" ]; then
  echo "cc-udon: no .c input" >&2
  exit 1
fi
if [ -z "$OUTPUT" ]; then
  echo "cc-udon: -o <output.uasm> is required" >&2
  exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# -ffreestanding: no hosted headers or startup files, which is what makes this
#                 work with no sysroot.
# -fno-builtin:  stops clang recognising the very functions libc.c defines,
#                 which would let it lower memset into a call to memset.
# -fno-strict-aliasing: the memory model is one byte array reinterpreted as
#                 words and structs; type-based aliasing assumptions are false.
# -fno-common:   a tentative definition must be one symbol, not a link-time
#                 merge, because the writer lays globals out by name.
# -include: a program gets the libc declarations without writing a #include,
#           and C99+ makes an implicit declaration a hard error, so without this
#           every libc call would need a prototype in the caller.
# -fno-vectorize / -fno-slp-vectorize: memory here is a byte array addressed by
#           byte offset, and a vectorised copy becomes a `<16 x i8>` load, which
#           is wider than any access this target can perform in one step. The
#           vectorizer is what turns an ordinary strncpy into one.
# The -fno-* flags must come AFTER the -O level: -O2 turns the vectorizer back
# on, so an earlier -fno-vectorize has no effect.
CFLAGS=(-ffreestanding -fno-builtin -fno-strict-aliasing -fno-common
        -O2 -S -emit-llvm -Wno-everything
        -fno-vectorize -fno-slp-vectorize -fno-unroll-loops
        -include "$ROOT/runtime/libc/udon_libc.h")

mkdir -p "$TMP"

# The program is compiled with every optimisation level the caller asks for by
# putting their flags in UDON_CFLAGS; the defaults are deliberately -O2 because
# the writer relies on SSA (PromoteMemoryToRegister) and on critical edges being
# split, and both are cheaper to get from the frontend's optimiser.
"$CLANG" "${CFLAGS[@]}" ${UDON_CFLAGS:-} -o "$TMP/program.ll" "$INPUT"

MODULES=("$TMP/program.ll")
if [ "${UDON_NO_LIBC:-0}" != "1" ]; then
  # No optimisation level here: CFLAGS already carries one, and a later -O2
  # would silently undo the -fno-vectorize that follows it.
  # The libc is built at -O1 on purpose: at -O2 clang turns a digit-range test
  # into a `switch i8` whose cases share a successor, and that shape currently
  # miscompiles (the loop walks off the end of memory). -O1 emits the same
  # compares for these functions and no jump table. See test/switch-shared.ll.
  "$CLANG" "${CFLAGS[@]}" -O1 -o "$TMP/libc.ll" "$ROOT/runtime/libc/libc.c"
  MODULES+=("$TMP/libc.ll")
fi

# One module, because the writer's call graph and slot layout are per module.
"$LLVM_LINK" -o "$TMP/linked.bc" "${MODULES[@]}"

"$LLVM_UDON" "$TMP/linked.bc" -o "$OUTPUT" "${PASSTHRU[@]}"

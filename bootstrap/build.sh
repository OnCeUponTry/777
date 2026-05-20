#!/usr/bin/env bash
# Pipeline stage0-posix para compilar C subset (M2-Planet) -> ELF AMD64.
# Si input es compilador_777.c, automaticamente concatena arch_<ARCH>.c antes
# (donde ARCH viene de --arch o default x86).
# Uso: ./build.sh <input.c> <output.elf> [--arch x86|arm]
set -eu

# Set STAGE0_POSIX to the absolute path of your stage0-posix clone.
# Get it from: https://github.com/oriansj/stage0-posix
#   git clone https://github.com/oriansj/stage0-posix
#   cd stage0-posix && bash kaem.amd64
# Then: export STAGE0_POSIX=/path/to/stage0-posix
S0=${STAGE0_POSIX:?STAGE0_POSIX env var required. See comments at top of this file.}
ARCH=amd64
BIN=$S0/AMD64/bin
M2LIBC=$S0/M2libc

IN=${1:?input.c requerido}
OUT=${2:?output.elf requerido}
ARCH_BACKEND="x86"  # default
# Parse --arch
shift 2
while [ $# -gt 0 ]; do
    case "$1" in
        --arch) ARCH_BACKEND="$2"; shift 2 ;;
        *) echo "Unknown flag: $1"; exit 1 ;;
    esac
done
TMP=$(mktemp -d)

export PATH=$BIN:$PATH

# Si compilamos compilador_777.c, necesitamos pasar arch_<X>.c ANTES.
EXTRA_FILES=""
if [ "$(basename "$IN")" = "compilador_777.c" ]; then
    ARCH_FILE="$(dirname "$IN")/arch_${ARCH_BACKEND}.c"
    if [ ! -f "$ARCH_FILE" ]; then
        echo "ERROR: arch file not found: $ARCH_FILE"
        exit 1
    fi
    EXTRA_FILES="-f $ARCH_FILE"
fi

# 1. M2-Planet: procesa headers + libc .c + arch backend + source en orden.
M2-Planet --architecture $ARCH \
    -f $M2LIBC/sys/types.h \
    -f $M2LIBC/stddef.h \
    -f $M2LIBC/$ARCH/linux/fcntl.c \
    -f $M2LIBC/fcntl.c \
    -f $M2LIBC/sys/utsname.h \
    -f $M2LIBC/$ARCH/linux/unistd.c \
    -f $M2LIBC/$ARCH/linux/sys/stat.c \
    -f $M2LIBC/ctype.c \
    -f $M2LIBC/stdlib.c \
    -f $M2LIBC/stdarg.h \
    -f $M2LIBC/stdio.h \
    -f $M2LIBC/stdio.c \
    -f $M2LIBC/string.c \
    -f $M2LIBC/bootstrappable.c \
    $EXTRA_FILES \
    -f $IN \
    --debug \
    -o $TMP/out.M1

# 2. blood-elf: genera stub debug info.
blood-elf --64 --little-endian -f $TMP/out.M1 -o $TMP/elf-stub.M1

# 3. M1: ensambla con libc-full (provee _start, __init_malloc, __init_io).
M1 --architecture $ARCH --little-endian \
    -f $M2LIBC/$ARCH/${ARCH}_defs.M1 \
    -f $M2LIBC/$ARCH/libc-full.M1 \
    -f $TMP/out.M1 \
    -f $TMP/elf-stub.M1 \
    -o $TMP/out.hex2

# 4. hex2: linker con ELF-amd64-debug.hex2.
hex2 --architecture $ARCH --little-endian \
    --base-address 0x00600000 \
    -f $M2LIBC/$ARCH/ELF-${ARCH}-debug.hex2 \
    -f $TMP/out.hex2 \
    -o $OUT

chmod 755 $OUT
SIZE=$(stat -c%s $OUT 2>/dev/null || stat -f%z $OUT)
echo "Built: $OUT ($SIZE bytes)"
/bin/rm -rf $TMP 2>/dev/null || true

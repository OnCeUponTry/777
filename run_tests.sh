#!/usr/bin/env bash
# Test runner para programas .777m sin Rust.
# Lee examples/test_manifest.txt (formato: name|exit_expected|stdout_expected_or_empty).
# Para cada test: compila con compilador_777.elf, ejecuta, valida.
#
# Salida:
#   PASS name (exit=N)
#   FAIL name <razon>
# Exit 0 si todos passan, 1 si hay fallos.

set -u

DIR=$(cd "$(dirname "$0")" && pwd)
# Default: usa gen0_compilador_777.elf (62 KB self-host fixed-point) que viene en el repo.
# Para usar el binario "full" compilado via stage0-posix:
#   COMPILER=$DIR/bootstrap/compilador_777.elf $DIR/run_tests.sh
COMPILER="${COMPILER:-$DIR/bootstrap/gen0_compilador_777.elf}"
EXAMPLES="$DIR/examples"
MANIFEST="$EXAMPLES/test_manifest.txt"

if [ ! -x "$COMPILER" ]; then
    echo "ERROR: compilador no encontrado o no ejecutable: $COMPILER"
    echo "Si quieres rebuildearlo desde source:"
    echo "  cd bootstrap && ./build.sh compilador_777.c compilador_777.elf"
    echo "  (requiere stage0-posix; ver README.md)"
    exit 2
fi
if [ ! -f "$MANIFEST" ]; then
    echo "ERROR: manifest no existe: $MANIFEST"
    exit 2
fi

PASS=0
FAIL=0
FAILS_LIST=()

while IFS='|' read -r name exit_expect stdout_expect stdin_input; do
    [ -z "$name" ] && continue
    src="$EXAMPLES/$name.777m"
    if [ ! -f "$src" ]; then
        echo "FAIL $name (source no encontrado)"
        FAIL=$((FAIL+1))
        FAILS_LIST+=("$name (no-source)")
        continue
    fi

    # Compilar
    tmp_elf=$(mktemp --suffix=.elf)
    if ! "$COMPILER" < "$src" > "$tmp_elf" 2>/dev/null; then
        echo "FAIL $name (compile error)"
        FAIL=$((FAIL+1))
        FAILS_LIST+=("$name (compile)")
        rm -f "$tmp_elf"
        continue
    fi
    chmod +x "$tmp_elf"

    # Ejecutar con timeout 5s safety. Si manifest provee stdin_input (4ta col),
    # se pipea al ELF; sino redirect a /dev/null para evitar consumir bytes
    # del manifest que esta siendo leido por este while.
    if [ -n "$stdin_input" ]; then
        actual_stdout=$(printf '%s' "$stdin_input" | timeout 5 "$tmp_elf" 2>/dev/null)
    else
        actual_stdout=$(timeout 5 "$tmp_elf" </dev/null 2>/dev/null)
    fi
    actual_exit=$?

    rm -f "$tmp_elf"

    # Validar
    ok=1
    reason=""
    if [ "$actual_exit" != "$exit_expect" ]; then
        ok=0
        reason="exit=$actual_exit esperado=$exit_expect"
    fi
    if [ -n "$stdout_expect" ] && [ "$actual_stdout" != "$stdout_expect" ]; then
        ok=0
        reason="${reason:+$reason; }stdout='$actual_stdout' esperado='$stdout_expect'"
    fi

    if [ "$ok" = "1" ]; then
        echo "PASS $name (exit=$actual_exit)"
        PASS=$((PASS+1))
    else
        echo "FAIL $name $reason"
        FAIL=$((FAIL+1))
        FAILS_LIST+=("$name")
    fi
done < "$MANIFEST"

echo ""
echo "=== Resultado ==="
echo "PASS: $PASS"
echo "FAIL: $FAIL"
if [ $FAIL -gt 0 ]; then
    echo "Fallos:"
    for f in "${FAILS_LIST[@]}"; do echo "  - $f"; done
    exit 1
fi
exit 0

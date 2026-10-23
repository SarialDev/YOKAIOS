#!/bin/sh
# Ruleaza ISO-ul de test headless si verifica markerii din tests/expect.txt.
# Uz: tests/run_qemu.sh <iso> [log]
# Cu EXPECT_PANIC="text": testul trece doar daca kernelul intra in panic cu acel text
set -u

ISO=${1:?iso lipsa}
LOG=${2:-build/test.log}
EXPECT=$(dirname "$0")/expect.txt
TIMEOUT=${TEST_TIMEOUT:-30}

mkdir -p "$(dirname "$LOG")"

timeout "$TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -m 512M \
    -nographic -serial stdio -monitor none -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    ${QEMU_EXTRA:-} < /dev/null > "$LOG.raw" 2>&1
rc=$?
tr -d '\r' < "$LOG.raw" > "$LOG"
rm -f "$LOG.raw"

fail=0
if [ -n "${EXPECT_PANIC:-}" ]; then
    # panic in modul test: outb(0xf4, 0x11) -> 35
    if [ $rc -ne 35 ]; then
        echo "FAIL: panic asteptat (cod 35), QEMU a iesit cu codul $rc"
        fail=1
    elif ! grep -q "PANIC: .*$EXPECT_PANIC" "$LOG"; then
        echo "FAIL: panic fara \"$EXPECT_PANIC\""
        fail=1
    fi
    if [ $fail -ne 0 ]; then
        echo "--- $LOG ---"
        cat "$LOG"
        exit 1
    fi
    echo "PASS (panic \"$EXPECT_PANIC\", log in $LOG)"
    exit 0
fi

# isa-debug-exit: outb(0xf4, 0x10) -> (0x10 << 1) | 1 = 33 inseamna succes
case $rc in
    33) ;;
    124) echo "FAIL: timeout dupa ${TIMEOUT}s"; fail=1 ;;
    *) echo "FAIL: QEMU a iesit cu codul $rc"; fail=1 ;;
esac

while IFS= read -r line || [ -n "$line" ]; do
    [ -z "$line" ] && continue
    if ! grep -qF -- "$line" "$LOG"; then
        echo "FAIL: lipseste \"$line\""
        fail=1
    fi
done < "$EXPECT"

if grep -q "PANIC" "$LOG"; then
    echo "FAIL: PANIC in log"
    fail=1
fi

if [ $fail -ne 0 ]; then
    echo "--- $LOG ---"
    cat "$LOG"
    exit 1
fi
echo "PASS ($(grep -c '^\[OK\]' "$LOG") markeri, log in $LOG)"
exit 0

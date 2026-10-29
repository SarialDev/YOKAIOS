#!/bin/sh
# Boot normal cu cmdline "debug=mouse", misca mouse-ul prin monitorul QEMU si verifica evenimentele tiparite pe serial.
# Uz: tests/run_mouse.sh <iso> [log]
set -u

ISO=${1:?iso lipsa}
LOG=${2:-build/mouse.log}
PORT=${MOUSE_MONITOR_PORT:-45454}
mkdir -p "$(dirname "$LOG")"
: > "$LOG"

qemu-system-x86_64 -cdrom "$ISO" -m 512M -smp 2 -display none -serial "file:$LOG" \
    -monitor "tcp:127.0.0.1:$PORT,server,nowait" -no-reboot ${QEMU_EXTRA:-} < /dev/null > /dev/null 2>&1 &
QPID=$!

cleanup(){ kill $QPID 2>/dev/null; wait $QPID 2>/dev/null; }

# Asteapta boot-ul complet (shell-ul tipareste promptul dupa [OK] mouse; folosim marcajul de pe serial)
i=0
while ! grep -q "ps2mouse:" "$LOG" 2>/dev/null; do
    i=$((i + 1))
    if [ $i -gt 300 ] || ! kill -0 $QPID 2>/dev/null; then
        echo "FAIL: kernelul nu a pornit mouse-ul"
        cat "$LOG"; cleanup; exit 1
    fi
    sleep 0.1
done
sleep 1

python3 - "$PORT" <<'PY'
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
def cmd(c):
    s.sendall((c + "\n").encode()); time.sleep(0.15)
time.sleep(0.3)
cmd("mouse_move 10 5")
cmd("mouse_move -7 -3")
cmd("mouse_button 1")
cmd("mouse_move 0 0 2")
cmd("mouse_button 0")
PY

sleep 1
cleanup

fail=0
check(){
    if ! grep -qF -- "$1" "$LOG"; then
        echo "FAIL: lipseste \"$1\""
        fail=1
    fi
}
check "[mouse] dx=10 dy=5 dz=0 buttons=0"
check "[mouse] dx=-7 dy=-3 dz=0 buttons=0"
check "buttons=1"
check "dz=-1"     # roata in sus (QEMU dz=2) => negativ
if grep -q "PANIC" "$LOG"; then
    echo "FAIL: PANIC in log"; fail=1
fi
if [ $fail -ne 0 ]; then
    echo "--- $LOG ---"; cat "$LOG"; exit 1
fi
echo "PASS ($(grep -c '^\[mouse\]' "$LOG") evenimente, log in $LOG)"

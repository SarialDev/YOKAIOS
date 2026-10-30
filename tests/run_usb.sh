#!/bin/sh
# USB fara PS/2: masina cu i8042=off, qemu-xhci + usb-kbd + usb-mouse, cmdline "debug=usb".
# Tasteaza "version" + Enter si misca mouse-ul prin monitorul QEMU; verifica evenimentele HID tiparite pe serial.
# Uz: tests/run_usb.sh <iso> [log]; SCREENDUMP=fisier.ppm salveaza ecranul la final
set -u

ISO=${1:?iso lipsa}
LOG=${2:-build/usb.log}
PORT=${USB_MONITOR_PORT:-45455}
mkdir -p "$(dirname "$LOG")"
: > "$LOG"

qemu-system-x86_64 -machine pc,i8042=off -cdrom "$ISO" -m 512M -smp 2 -display none -serial "file:$LOG" \
    -device qemu-xhci -device usb-kbd -device usb-mouse \
    -monitor "tcp:127.0.0.1:$PORT,server,nowait" -no-reboot ${QEMU_EXTRA:-} < /dev/null > /dev/null 2>&1 &
QPID=$!

cleanup(){ kill $QPID 2>/dev/null; wait $QPID 2>/dev/null; }

i=0
while ! grep -q "^\[OK\] usb" "$LOG" 2>/dev/null; do
    i=$((i + 1))
    if [ $i -gt 300 ] || ! kill -0 $QPID 2>/dev/null; then
        echo "FAIL: kernelul nu a pornit USB"
        cat "$LOG"; cleanup; exit 1
    fi
    sleep 0.1
done
sleep 2

python3 - "$PORT" "${SCREENDUMP:-}" <<'PY'
import socket, sys, time
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])))
def cmd(c, t=0.15):
    s.sendall((c + "\n").encode()); time.sleep(t)
time.sleep(0.3)
for k in "version":
    cmd("sendkey " + k)
cmd("sendkey ret", 0.5)
cmd("sendkey shift-a")
cmd("mouse_move 10 5")
cmd("mouse_move -7 -3")
cmd("mouse_button 1")
cmd("mouse_move 0 0 2")
cmd("mouse_button 0", 0.5)
if sys.argv[2]:
    cmd("screendump " + sys.argv[2], 1)
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
check "usb: hid tastatura"
check "usb: hid mouse"
# v e r s i o n Enter (ascii zecimal), apasari
for a in 118 101 114 115 105 111 110 10; do
    check "ascii=$a pressed=1"
done
check "ascii=65 pressed=1 mods=1"     # Shift+A
check "[mouse] dx=10 dy=5 dz=0 buttons=0"
check "[mouse] dx=-7 dy=-3 dz=0 buttons=0"
check "buttons=1"
check "dz=-1"
if grep -q "PANIC" "$LOG"; then
    echo "FAIL: PANIC in log"; fail=1
fi
if [ $fail -ne 0 ]; then
    echo "--- $LOG ---"; cat "$LOG"; exit 1
fi
echo "PASS ($(grep -c '^\[usbkbd\]' "$LOG") taste, $(grep -c '^\[mouse\]' "$LOG") pachete mouse, log in $LOG)"

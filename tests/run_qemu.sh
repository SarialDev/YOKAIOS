#!/bin/sh
# Ruleaza ISO-ul de test headless si verifica markerii din tests/expect.txt.
# Uz: tests/run_qemu.sh <iso> [log]; EXPECT_FILE alege alta lista de markeri
# Cu EXPECT_PANIC="text": testul trece doar daca kernelul intra in panic cu acel text
# FB=0: boot cu "nofb" (consola VGA text); liniile "@fb " / "@nofb " din expect.txt se cer doar cu / fara framebuffer
# Cand kernelul tipareste "drivers: astept intrare" (test=drivers), scriptul tasteaza "version<ret>" si "cat /etc/hostname<ret>" prin monitorul QEMU
# USB=0: fara xHCI; QEMU_EXTRA="-machine pc,i8042=off": fara PS/2 (tastatura doar prin usb-kbd)
# SMP=n alege numarul de CPU-uri (implicit 4); expect.txt: @SMP@ = n, prefixul "@smp>1 " = doar cu 2+ CPU-uri
set -u

ISO=${1:?iso lipsa}
LOG=${2:-build/test.log}
EXPECT=${EXPECT_FILE:-$(dirname "$0")/expect.txt}
TIMEOUT=${TEST_TIMEOUT:-300}
SMP=${SMP:-4}

mkdir -p "$(dirname "$LOG")"

# USB=0: fara controller xHCI (doar tastatura PS/2)
USB_DEVICES="-device qemu-xhci -device usb-kbd -device usb-mouse"
[ "${USB:-1}" = 0 ] && USB_DEVICES=""
SOCK=${TMPDIR:-/tmp}/yokai-mon-$$.sock
INPUT_MARK="drivers: astept intrare"
rm -f "$SOCK"

timeout "$TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -m 512M -smp "$SMP" \
    -display none -serial "file:$LOG.raw" -monitor "unix:$SOCK,server,nowait" -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -nic user,model=e1000 -device edu -netdev user,id=vn0 -device virtio-net-pci,netdev=vn0 \
    $USB_DEVICES \
    ${QEMU_EXTRA:-} < /dev/null > "$LOG.qemu" 2>&1 &
QPID=$!

# Cand kernelul tipareste markerul "astept intrare" (test=drivers), trimite "version<ret>" prin monitorul QEMU
(
    while ! grep -q "$INPUT_MARK" "$LOG.raw" 2>/dev/null; do
        kill -0 $QPID 2>/dev/null || exit 0
        sleep 0.2
    done
    sleep 0.5
    python3 - "$SOCK" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.3)
# "version", apoi "cat /etc/hostname" (citit din initrd prin VFS)
for k in ["v", "e", "r", "s", "i", "o", "n", "ret", "wait",
          "c", "a", "t", "spc", "slash", "e", "t", "c", "slash", "h", "o", "s", "t", "n", "a", "m", "e", "ret"]:
    if k == "wait":
        time.sleep(1.0)
        continue
    s.sendall(("sendkey " + k + "\n").encode())
    time.sleep(0.2)
PY
) &
SENDER=$!

wait $QPID
rc=$?
kill $SENDER 2>/dev/null
rm -f "$SOCK"
tr -d '\r' < "$LOG.raw" > "$LOG"
rm -f "$LOG.raw" "$LOG.qemu"

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
    # "@SMP@" = numarul de CPU-uri; liniile "@smp>1 " exista doar cu cel putin 2 CPU-uri
    case "$line" in
        "@smp>1 "*) [ "$SMP" -lt 2 ] && continue; line=${line#@smp>1 } ;;
        "@fb "*) [ "${FB:-1}" = 0 ] && continue; line=${line#@fb } ;;
        "@nofb "*) [ "${FB:-1}" = 1 ] && continue; line=${line#@nofb } ;;
    esac
    line=$(printf '%s' "$line" | sed "s/@SMP@/$SMP/g")
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

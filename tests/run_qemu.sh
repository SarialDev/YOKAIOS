#!/bin/sh
# Ruleaza ISO-ul de test headless si verifica markerii din tests/expect.txt.
# Uz: tests/run_qemu.sh <iso> [log]; EXPECT_FILE alege alta lista de markeri
# Cu EXPECT_PANIC="text": testul trece doar daca kernelul intra in panic cu acel text
# FB=0: boot cu "nofb" (consola VGA text); liniile "@fb " / "@nofb " din expect.txt se cer doar cu / fara framebuffer
# Cand kernelul tipareste "drivers: astept intrare" (test=drivers), scriptul tasteaza "version<ret>" prin monitorul QEMU
# (kernelul il citeste de pe tty); la "ysh: astept intrare" (test=ysh) ruleaza tests/ysh_test.py run, care scrie un script
# intreg in ysh (editare, istoric, Tab, pipe-uri, redirectari) si asteapta rezultatul fiecarei comenzi in log
# USB=0: fara xHCI; QEMU_EXTRA="-machine pc,i8042=off": fara PS/2 (tastatura doar prin usb-kbd)
# DISK=<imagine>: disc SATA (AHCI, format raw) cu imaginea data; boot de pe CD (-boot order=d). Imaginea o poate modifica kernelul
# NIC="..." inlocuieste placa implicita (-nic user,model=e1000), ex. cu hostfwd sau -netdev socket (test-lwip)
# HOST_CMD="comanda": se ruleaza pe gazda cand kernelul tipareste "astept host" (HOST_MARK schimba textul); iesirea ei
# merge in <log>.host, iar un cod nenul pica testul (test-lwip: clientul TCP/UDP, peer-ul de ping)
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
DISK_DEVICES=""
[ -n "${DISK:-}" ] && DISK_DEVICES="-boot order=d -device ahci,id=ahci0 -drive file=$DISK,if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ahci0.0"
SOCK=${TMPDIR:-/tmp}/yokai-mon-$$.sock
INPUT_MARK="drivers: astept intrare"
YSH_MARK="ysh: astept intrare"
rm -f "$SOCK"

timeout "$TIMEOUT" qemu-system-x86_64 -cdrom "$ISO" -m 512M -smp "$SMP" \
    -display none -serial "file:$LOG.raw" -monitor "unix:$SOCK,server,nowait" -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    ${NIC:--nic user,model=e1000} -device edu -netdev user,id=vn0 -device virtio-net-pci,netdev=vn0 \
    $USB_DEVICES $DISK_DEVICES \
    ${QEMU_EXTRA:-} < /dev/null > "$LOG.qemu" 2>&1 &
QPID=$!

# Cand kernelul tipareste un marker de intrare, trimite tastele prin monitorul QEMU
(
    MARK=""
    while [ -z "$MARK" ]; do
        kill -0 $QPID 2>/dev/null || exit 0
        if grep -q "$INPUT_MARK" "$LOG.raw" 2>/dev/null; then
            MARK=drivers
        elif grep -q "$YSH_MARK" "$LOG.raw" 2>/dev/null; then
            MARK=ysh
        else
            sleep 0.2
        fi
    done
    sleep 0.5
    if [ "$MARK" = ysh ]; then
        python3 "$(dirname "$0")/ysh_test.py" run "$SOCK" "$LOG.raw"
        exit 0
    fi
    python3 - "$SOCK" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.3)
for k in ["v", "e", "r", "s", "i", "o", "n", "ret"]:
    s.sendall(("sendkey " + k + "\n").encode())
    time.sleep(0.2)
PY
) &
SENDER=$!

HELPER=""
if [ -n "${HOST_CMD:-}" ]; then
    (
        while ! grep -q "${HOST_MARK:-astept host}" "$LOG.raw" 2>/dev/null; do
            kill -0 $QPID 2>/dev/null || exit 0
            sleep 0.2
        done
        sh -c "$HOST_CMD" > "$LOG.host" 2>&1
    ) &
    HELPER=$!
fi

wait $QPID
rc=$?
kill $SENDER 2>/dev/null
rm -f "$SOCK"
tr -d '\r' < "$LOG.raw" > "$LOG"
rm -f "$LOG.raw" "$LOG.qemu"

fail=0
if [ -n "$HELPER" ]; then
    wait $HELPER
    hrc=$?
    if [ -f "$LOG.host" ]; then
        cat "$LOG.host"
    fi
    if [ $hrc -ne 0 ]; then
        echo "FAIL: HOST_CMD a iesit cu codul $hrc"
        fail=1
    fi
fi

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

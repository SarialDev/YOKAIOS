#!/bin/sh
# ext4 cu scriere si jurnal (test=ext4w): mai multe boot-uri QEMU pe acelasi disc, cu oprire brusca intre ele.
# Ce face fiecare boot scrie scriptul in primul sector al lui sda1 ("ext4w mode=... ", vezi kernel/tests/test_ext4w.h).
# Uz: tests/check_ext4w.sh <iso> <disc ext4-ext4.img> <caz> [prefix iesire]
#   crash1..crash5   S1 comisa, S2 intrerupta la etapa N a commit-ului (outb isa-debug-exit); la boot-ul 2 replay-ul
#                    trebuie sa dea exact S1 (1, 5) sau S2 (2, 3, 4)
#   clean            S2 comisa si umount curat, apoi remontare
#   kill             scrieri la intamplare cu commit=1, QEMU omorat cu SIGKILL dupa cateva secunde
#   replay, replay-csum
#                    jurnal scris de debugfs (2 tranzactii + revoke, cu / fara checksum v3) reluat de kernel; kernelul
#                    scrie apoi si cade dupa commit, iar boot-ul 3 reia jurnalul lui
# Dupa ultimul boot: e2fsck -fn curat (fara needs_recovery) si liniile EXT4W ale kernelului = continutul citit de
# debugfs. Imaginea de dupa cadere e reluata si de e2fsprogs (e2fsck -fy pe o copie): rezultatul trebuie sa fie curat
# si identic cu ce a vazut kernelul dupa propriul replay (liniile EXT4S / EXT4W).
set -eu

ISO=${1:?iso lipsa}
BASE=${2:?disc lipsa}
CASE=${3:?caz lipsa}
OUT=${4:-build/ext4w-$CASE}
here=$(dirname "$0")
PSTART=18432
PCOUNT=114688
CTL=2048                        # sda1
DISK=$OUT.img
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
EXT4="-boot order=d -device ahci,id=ahci0 -drive file=$DISK,if=none,id=d0,format=raw -device ide-hd,drive=d0,bus=ahci0.0"

fail(){
    echo "FAIL ($CASE): $*"
    exit 1
}

ctl(){
    printf 'ext4w %s\n' "$1" | dd of="$DISK" bs=512 seek=$CTL count=1 conv=notrunc,sync status=none
}

part(){
    dd if="$DISK" of="$1" bs=512 skip=$PSTART count=$PCOUNT conv=sparse status=none
}

unpart(){
    dd if="$1" of="$DISK" bs=512 seek=$PSTART conv=notrunc status=none
}

boot(){
    TEST_TIMEOUT=${TEST_TIMEOUT:-900} EXPECT_FILE="$here/$2" QEMU_EXTRA="$EXT4 ${QEMU_EXTRA:-}" \
        sh "$here/run_qemu.sh" "$ISO" "$1" || fail "boot $1"
}

has(){
    grep -qF -- "$2" "$1" || fail "lipseste \"$2\" in $1"
}

# e2fsck -fn curat si fara needs_recovery pe partitia discului
fsck_clean(){
    part "$W/final.img"
    e2fsck -fn "$W/final.img" > "$W/fsck.log" 2>&1 || { cat "$W/fsck.log"; fail "e2fsck -fn a gasit erori"; }
    if dumpe2fs -h "$W/final.img" 2> /dev/null | grep -q '^Filesystem features:.*needs_recovery'; then
        fail "needs_recovery ramas dupa umount"
    fi
    echo "e2fsck -fn curat: $(tail -1 "$W/fsck.log")"
}

# Copia de dupa cadere, reluata de e2fsprogs: doar mesajele obisnuite ale unui replay, apoi -fn curat
linux_replay(){
    rc=0
    e2fsck -fy "$1" > "$W/fsckr.log" 2>&1 || rc=$?
    [ $rc -le 1 ] || { cat "$W/fsckr.log"; fail "e2fsck -fy pe copia cazuta: cod $rc"; }
    if grep -vE '^(e2fsck [0-9]|.*: recovering journal|Pass [1-5][A-Z]?:|Clearing orphaned inode|.*: \*+ FILE SYSTEM WAS MODIFIED \*+|.*: [0-9]+/[0-9]+ files .*blocks$|Truncating orphaned inode|Setting free (inodes|blocks) count|Clearing orphaned inode|$)' "$W/fsckr.log" > "$W/fsckr.extra"; then
        cat "$W/fsckr.log"
        fail "e2fsck a reparat ceva dupa replay (vezi mai sus)"
    fi
    e2fsck -fn "$1" > "$W/fsck2.log" 2>&1 || { cat "$W/fsck2.log"; fail "e2fsck -fn dupa replay-ul e2fsprogs"; }
    echo "replay e2fsprogs: $(grep -c 'recovering journal' "$W/fsckr.log") replay, e2fsck -fn curat"
}

# KEEP=1: partitia de dupa cadere ramane in <prefix>-crashed.part (pentru depanare)
keep(){
    [ "${KEEP:-0}" = 1 ] && cp --sparse=always "$W/crashed.img" "$OUT-crashed.part"
    return 0
}

check(){
    python3 "$here/ext4w_check.py" "$1" "$2" "$3" || fail "liniile $3 difera de disc ($1)"
}

cp --sparse=always "$BASE" "$DISK"
case $CASE in
crash[0-9]|clean)
    N=${CASE#crash}
    [ "$CASE" = clean ] && N=0
    ctl "mode=write crash=$N"
    boot "$OUT-1.log" expect-ext4w-write.txt
    if [ "$N" = 0 ]; then
        has "$OUT-1.log" "[OK] ext4w umount curat"
    else
        has "$OUT-1.log" "[OK] ext4w cadere la etapa $N"
        if grep -q "\[OK\] boot" "$OUT-1.log"; then
            fail "kernelul a continuat dupa cadere"
        fi
    fi
    part "$W/crashed.img"
    keep
    ctl "mode=verify crash=$N"
    boot "$OUT-2.log" expect-ext4w-verify.txt
    fsck_clean
    check "$W/final.img" "$OUT-2.log" EXT4W
    if [ "$N" != 0 ]; then
        linux_replay "$W/crashed.img"
        check "$W/crashed.img" "$OUT-2.log" EXT4S
    fi
    ;;
kill)
    SEED=${SEED:-$(od -An -N2 -tu2 /dev/urandom | tr -d ' ')}
    DELAY=$((SEED % 10 + 3))
    ctl "mode=stress seed=$SEED"
    LOG=$OUT-1.log
    rm -f "$LOG.raw"
    qemu-system-x86_64 -cdrom "$ISO" -m 512M -smp "${SMP:-4}" -display none -serial "file:$LOG.raw" -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 $EXT4 ${QEMU_EXTRA:-} < /dev/null > "$LOG.qemu" 2>&1 &
    QPID=$!
    t=0
    while ! grep -q "ext4w stres pornit" "$LOG.raw" 2> /dev/null; do
        kill -0 $QPID 2> /dev/null || { cat "$LOG.raw" "$LOG.qemu"; fail "QEMU s-a oprit inainte de stres"; }
        t=$((t + 1))
        [ $t -lt 3000 ] || { kill -9 $QPID; fail "stresul nu a pornit"; }
        sleep 0.2
    done
    sleep $DELAY
    kill -9 $QPID 2> /dev/null || fail "QEMU nu mai rula la kill"
    wait $QPID 2> /dev/null || true
    tr -d '\r' < "$LOG.raw" > "$LOG"
    rm -f "$LOG.raw" "$LOG.qemu"
    if grep -q PANIC "$LOG"; then
        cat "$LOG"
        fail "PANIC in timpul stresului"
    fi
    echo "kill -9 dupa ${DELAY}s de stres (seed $SEED): $(grep 'ext4w stres:' "$LOG" | tail -1)"
    part "$W/crashed.img"
    keep
    ctl "mode=stresscheck"
    boot "$OUT-2.log" expect-ext4w-stresscheck.txt
    fsck_clean
    check "$W/final.img" "$OUT-2.log" EXT4S
    linux_replay "$W/crashed.img"
    check "$W/crashed.img" "$OUT-2.log" EXT4S
    ;;
replay|replay-csum)
    C=0
    JO=jo
    if [ "$CASE" = replay-csum ]; then
        C=1
        JO="jo -c"
    fi
    part "$W/p.img"
    B0=$(debugfs -R "bmap /data/big.bin 0" "$W/p.img" 2> /dev/null)
    B1=$(debugfs -R "bmap /data/big.bin 1" "$W/p.img" 2> /dev/null)
    python3 -c "import sys; open(sys.argv[1], 'wb').write(b'\xa5' * 8192); open(sys.argv[2], 'wb').write(b'\x5a' * 4096)" \
        "$W/a5" "$W/5a"
    # tranzactia 1: blocurile 0, 1 din big.bin = 0xA5; tranzactia 2: blocul 0 = 0x5A si revoke pentru blocul 1
    printf '%s\njw -b %s,%s %s\njc\njo\njw -b %s -r %s %s\njc\n' "$JO" "$B0" "$B1" "$W/a5" "$B0" "$B1" "$W/5a" \
        | debugfs -w -f - "$W/p.img" > "$W/jw.log" 2>&1
    debugfs -R logdump "$W/p.img" 2> /dev/null | grep -q "type 5 (revoke table)" || { cat "$W/jw.log"; fail "debugfs nu a scris jurnalul"; }
    unpart "$W/p.img"
    ctl "mode=replay csum=$C"
    boot "$OUT-1.log" expect-ext4w-replay.txt
    part "$W/crashed.img"
    keep
    ctl "mode=replay2 csum=$C"
    boot "$OUT-2.log" expect-ext4w-replay2.txt
    fsck_clean
    check "$W/final.img" "$OUT-2.log" EXT4W
    linux_replay "$W/crashed.img"
    check "$W/crashed.img" "$OUT-2.log" EXT4W
    ;;
*)
    fail "caz necunoscut"
    ;;
esac
echo "PASS ext4w $CASE"

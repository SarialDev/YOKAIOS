#!/bin/sh
# Dupa test=fat32: imaginea modificata de kernel trebuie sa fie curata pentru fsck.fat si sa contina ce a lasat testul.
# Uz: tests/check_fat32.sh <imagine>   (partitia incepe la sectorul 2048, ca in tests/mkfat32.sh)
set -eu

IMG=${1:?imagine lipsa}
START=2048
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export MTOOLS_SKIP_CHECK=1
MT="-i $IMG@@$((START * 512))"
fail=0

dd if="$IMG" of="$W/part.img" bs=512 skip=$START 2> /dev/null
if ! fsck.fat -n -v "$W/part.img" > "$W/fsck.log" 2>&1; then
    echo "FAIL: fsck.fat a raportat erori"
    cat "$W/fsck.log"
    exit 1
fi

check() {
    if ! "$@"; then
        echo "FAIL: $*"
        fail=1
    fi
}

mtype $MT ::PERSIST.TXT > "$W/persist.txt"
check [ "$(cat "$W/persist.txt")" = "scris de YokaiOS" ]
mtype $MT "::Kept Directory/kept file.txt" > "$W/kept.txt"
check [ "$(cat "$W/kept.txt")" = "kept" ]
mtype $MT "::Renamed Directory/c/g.txt" > "$W/g.txt"
check [ "$(cat "$W/g.txt")" = "after move" ]
mtype $MT "::Renamed Directory/f2.txt" > "$W/f2.txt"
check [ "$(cat "$W/f2.txt")" = "in c" ]
mtype $MT ::HELLO.TXT > "$W/hello.txt"
check [ "$(cat "$W/hello.txt")" = "hello from mkfs.fat" ]

mcopy $MT ::persist.bin "$W/persist.bin"
mcopy $MT ::big.bin "$W/big.bin"
python3 - "$W/persist.bin" "$W/big.bin" <<'PY' || fail=1
import sys
def pat(n, seed):
    return bytes((i * 131 + (i >> 9) + seed) & 0xFF for i in range(n))
p = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
if p != pat(70000, 7):
    sys.exit("persist.bin difera")
if b != pat(300000, 0):
    sys.exit("big.bin difera")
PY

# Fisierele sterse nu mai exista, cele lasate sunt in "names" (2 intrari)
if mdir $MT ::a > /dev/null 2>&1; then echo "FAIL: ::a ar fi trebuit sters"; fail=1; fi
if mdir $MT ::many > /dev/null 2>&1; then echo "FAIL: ::many ar fi trebuit sters"; fail=1; fi
if mdir $MT ::grow.bin > /dev/null 2>&1; then echo "FAIL: ::grow.bin ar fi trebuit sters"; fail=1; fi
n=$(mdir -b $MT ::names | wc -l)
check [ "$n" -eq 2 ]
# Nume lungi Unicode scrise de kernel: mtools le vede ca nume Unicode
mdir $MT ::names > "$W/names.txt"
check grep -q "lung" "$W/names.txt"

[ $fail -eq 0 ] || exit 1
tail -3 "$W/fsck.log"
echo "PASS (fsck.fat curat, continutul lasat de kernel verificat cu mtools)"

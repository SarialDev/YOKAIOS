#!/bin/sh
# Dupa test=ext4: sistemul de fisiere trebuie sa fie curat pentru e2fsck (kernelul nu scrie) si arborele tiparit de
# kernel (linii "EXT4 ...": CRC32, dimensiune, mod) sa fie identic cu cel din sursa (tests/ext4_list.py).
# Uz: tests/check_ext4.sh <imagine> <log> <ok|recover|refuse>   (imaginea ok are lista in <imagine>.list)
set -eu

IMG=${1:?imagine lipsa}
LOG=${2:?log lipsa}
MODE=${3:-ok}
LIST=${4:-$IMG.list}
PSTART=18432
PCOUNT=114688
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

if [ "$MODE" = refuse ]; then
    grep -q "flag-uri incompatibile nesuportate" "$LOG" || { echo "FAIL: kernelul nu a refuzat flag-ul necunoscut"; exit 1; }
    echo "PASS (flag INCOMPAT necunoscut refuzat, radacina initrd)"
    exit 0
fi
if [ "$MODE" = ok ]; then
    dd if="$IMG" of="$W/part.img" bs=512 skip=$PSTART count=$PCOUNT 2> /dev/null
    if ! e2fsck -fn "$W/part.img" > "$W/fsck.log" 2>&1; then
        echo "FAIL: e2fsck a raportat erori"
        cat "$W/fsck.log"
        exit 1
    fi
fi
grep '^EXT4 ' "$LOG" | LC_ALL=C sort > "$W/got.txt"
LC_ALL=C sort "$LIST" > "$W/want.txt"
if ! cmp -s "$W/got.txt" "$W/want.txt"; then
    echo "FAIL: arborele citit de kernel difera de sursa"
    diff "$W/want.txt" "$W/got.txt" | head -20
    exit 1
fi
echo "PASS (e2fsck curat, $(wc -l < "$W/got.txt") intrari: continut, CRC32, moduri si symlink-uri identice cu sursa)"

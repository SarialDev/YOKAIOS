#!/bin/sh
# ext4/jbd2 pe gazda (codul kernelului compilat cu ASAN/UBSan, disc simulat in memorie, fara QEMU):
#   model  operatii aleatoare comparate cu un model in memorie, apoi remontare si e2fsck -fn
#   sweep  sarcina de lucru fixa intrerupta la fiecare a STEP-a scriere; la cadere se aplica un subset aleator al
#          scrierilor neconfirmate de flush, in ordine aleatoare. Dupa replay arborele trebuie sa fie una din starile
#          comise si e2fsck -fn curat
# Uz: tests/host/run.sh <disc ext4-ext4.img> [STEP]
set -eu

DISK=${1:?disc lipsa}
STEP=${2:-41}
here=$(dirname "$0")
BIN=build/ext4_host
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT

sh "$here/build.sh"
dd if="$DISK" of="$W/base.img" bs=512 skip=18432 count=114688 conv=sparse status=none
for seed in 1 2 3 4; do
    cp --sparse=always "$W/base.img" "$W/m.img"
    "$BIN" model "$W/m.img" $seed 2000 > "$W/run.log" 2>&1 || { tail -20 "$W/run.log"; echo "FAIL: model seed $seed"; exit 1; }
    grep '^model:' "$W/run.log"
    e2fsck -fn "$W/m.img" > "$W/fsck.log" 2>&1 || { cat "$W/fsck.log"; echo "FAIL: e2fsck dupa model seed $seed"; exit 1; }
done
"$BIN" sweep "$W/base.img" "$STEP" 7 > "$W/run.log" 2>&1 || { grep -v 'jurnal reluat' "$W/run.log" | tail -20; echo "FAIL: sweep"; exit 1; }
grep '^sweep:' "$W/run.log"
echo "PASS ext4 pe gazda (model + sweep, pas $STEP)"

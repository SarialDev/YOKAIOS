#!/bin/sh
# Imagine de disc de 64 MiB cu MBR si o partitie FAT32 (sda1), facuta cu mkfs.fat si populata cu mtools.
# Uz: tests/mkfat32.sh <imagine> [sectoare_pe_cluster]   (1 = clustere de 512 B, 8 = 4 KiB)
set -eu

IMG=${1:?imagine lipsa}
SPC=${2:-1}
START=2048
COUNT=129024                # 63 MiB: 131072 de sectoare - 2048

rm -f "$IMG"
truncate -s 64M "$IMG"
python3 - "$IMG" "$START" "$COUNT" <<'PY'
import struct, sys
img, start, count = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(img, "r+b") as f:
    mbr = bytearray(512)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x00\x00", 0x0C, b"\x00\x00\x00", start, count)
    mbr[510:512] = b"\x55\xAA"
    f.write(mbr)
PY
mkfs.fat -F 32 -s "$SPC" -n YOKAIFAT --offset "$START" "$IMG" $((COUNT / 2)) > /dev/null

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
MT="-i $IMG@@$((START * 512))"
printf 'hello from mkfs.fat\n' > "$W/HELLO.TXT"
printf 'long name via mtools\n' > "$W/Long File Name Example.txt"
printf 'nested file\n' > "$W/nested.txt"
printf 'deep\n' > "$W/deep file name.txt"
python3 - "$W/big.bin" <<'PY'
import sys
open(sys.argv[1], "wb").write(bytes((i * 131 + (i >> 9)) & 0xFF for i in range(300000)))
PY
export MTOOLS_SKIP_CHECK=1
mcopy $MT "$W/HELLO.TXT" ::HELLO.TXT
mcopy $MT "$W/Long File Name Example.txt" "::Long File Name Example.txt"
mcopy $MT "$W/big.bin" ::big.bin
mmd $MT ::dir1
mcopy $MT "$W/nested.txt" ::dir1/nested.txt
mmd $MT "::Mixed Case Dir"
mcopy $MT "$W/deep file name.txt" "::Mixed Case Dir/deep file name.txt"

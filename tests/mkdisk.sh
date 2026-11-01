#!/bin/sh
# Disc de test pentru stocare (make test-fs): 114 MiB, tabela GPT cu
#   sda1  48 MiB  FAT32 (mkfs.fat -s 1, clustere de 512 B, populat cu mtools)
#   sda2  64 MiB  ext4 (mke2fs -d: 64bit, metadata_csum, extente, jurnal)
# Ambele au aceleasi nume de fisiere, dar continut diferit (pseudo-aleator, determinist), si un MANIFEST.SHA in radacina
# (linii "<sha256> <dimensiune> <cale>"); copiile manifestelor raman langa imagine: <prefix>.fat32.sha, <prefix>.ext4.sha.
# Uz: tests/mkdisk.sh <imagine> [prefix]   (prefix implicit = imagine fara extensie). Necesita mkfs.fat, mtools,
# e2fsprogs si python3
set -eu

IMG=${1:?imagine lipsa}
PREFIX=${2:-${IMG%.*}}
IMG=$(cd "$(dirname "$IMG")" && pwd)/$(basename "$IMG")      # absolut: mai jos se face cd
P1_START=2048
P1_COUNT=98304                  # 48 MiB
P2_START=$((P1_START + P1_COUNT))
P2_COUNT=131072                 # 64 MiB
SECTORS=$((P2_START + P2_COUNT + 2048))

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
mkdir "$W/fat32" "$W/ext4"

# Fisierele: 15 pe fiecare sistem, SHA-256 din hashlib; continutul vine dintr-un flux sha256(seed || contor)
python3 - "$W" <<'PY'
import hashlib, os, sys

w = sys.argv[1]
SIZES = [1, 511, 512, 513, 4095, 4096, 4097, 65536, 65537, 1048589]


def stream(tag, n):
    out = bytearray()
    c = 0
    while len(out) < n:
        out += hashlib.sha256(b"%s/%d" % (tag.encode(), c)).digest()
        c += 1
    return bytes(out[:n])


for fs in ("fat32", "ext4"):
    files = {
        "hello.txt": ("hello from %s, known content\n" % fs).encode(),
        "empty.dat": b"",
        "docs/readme.txt": ("docs of %s\n" % fs * 40).encode(),
        "docs/sub/deep/nested.bin": stream(fs + "/nested", 100000),
        "Mixed Case Long Name.txt": ("long name on %s\n" % fs).encode(),
    }
    for s in SIZES:
        files["data/size_%07d.bin" % s] = stream("%s/size/%d" % (fs, s), s)
    lines = []
    for rel, data in sorted(files.items()):
        p = os.path.join(w, fs, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as f:
            f.write(data)
        lines.append("%s %d %s\n" % (hashlib.sha256(data).hexdigest(), len(data), rel))
    with open(os.path.join(w, fs, "MANIFEST.SHA"), "w") as f:
        f.writelines(lines)
PY

# --- FAT32 ---
rm -f "$IMG"
truncate -s $((SECTORS * 512)) "$IMG"
python3 - "$IMG" "$SECTORS" "$P1_START" "$P1_COUNT" "$P2_START" "$P2_COUNT" <<'PY'
import struct, sys, uuid, zlib

img, total, p1s, p1c, p2s, p2c = sys.argv[1], *map(int, sys.argv[2:7])
SS = 512
BASIC = uuid.UUID("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7").bytes_le
LINUX = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4").bytes_le
ents = bytearray(128 * 128)
for i, (typ, first, count, name) in enumerate(((BASIC, p1s, p1c, "YOKAIFAT"), (LINUX, p2s, p2c, "YOKAIEXT"))):
    e = bytearray(128)
    e[0:16] = typ
    e[16:32] = uuid.UUID(int=0x594F4B41490000000000000000000000 + i + 1).bytes_le
    struct.pack_into("<QQQ", e, 32, first, first + count - 1, 0)
    e[56:56 + 2 * len(name)] = name.encode("utf-16-le")
    ents[128 * i:128 * i + 128] = e
tcrc = zlib.crc32(bytes(ents)) & 0xFFFFFFFF
table_sectors = len(ents) // SS


def header(my, alt, table_lba):
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    struct.pack_into("<IIIIQQQQ", h, 8, 0x00010000, 92, 0, 0, my, alt, 2 + table_sectors - 0, total - 2 - table_sectors)
    h[56:72] = uuid.UUID(int=0x594F4B41490000000000000000000001).bytes_le
    struct.pack_into("<QIII", h, 72, table_lba, 128, 128, tcrc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
    return bytes(h).ljust(SS, b"\0")


mbr = bytearray(SS)
mbr[446 + 4] = 0xEE
struct.pack_into("<II", mbr, 446 + 8, 1, min(total - 1, 0xFFFFFFFF))
mbr[510:512] = b"\x55\xAA"
with open(img, "r+b") as f:
    f.write(mbr)
    f.seek(SS)
    f.write(header(1, total - 1, 2))
    f.write(ents)
    f.seek((total - 1 - table_sectors) * SS)
    f.write(ents)
    f.write(header(total - 1, 1, total - 1 - table_sectors))
PY
mkfs.fat -F 32 -s 1 -n YOKAIFAT --offset "$P1_START" "$IMG" $((P1_COUNT / 2)) > /dev/null 2>&1
export MTOOLS_SKIP_CHECK=1
MT="-i $IMG@@$((P1_START * 512))"
(cd "$W/fat32" && find . -type d ! -name . | sed 's|^\./||' | sort | while read -r d; do mmd $MT "::$d"; done)
(cd "$W/fat32" && find . -type f | sed 's|^\./||' | sort | while read -r f; do mcopy $MT "$f" "::$f"; done)

# --- ext4 ---
mke2fs -q -F -t ext4 -b 4096 -L YOKAIEXT -O 64bit,metadata_csum,flex_bg,dir_index,extent \
    -E root_owner=0:0 -d "$W/ext4" "$W/ext4.img" $((P2_COUNT / 2))k
dd if="$W/ext4.img" of="$IMG" bs=512 seek="$P2_START" conv=notrunc status=none

cp "$W/fat32/MANIFEST.SHA" "$PREFIX.fat32.sha"
cp "$W/ext4/MANIFEST.SHA" "$PREFIX.ext4.sha"
echo "mkdisk: $IMG ($((SECTORS / 2048)) MiB GPT: sda1 FAT32 $P1_START+$P1_COUNT, sda2 ext4 $P2_START+$P2_COUNT)"

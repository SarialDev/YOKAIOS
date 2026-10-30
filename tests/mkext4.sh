#!/bin/sh
# Imagine de disc cu MBR: sda1 (8 MiB, gol) si sda2 = ext4 (56 MiB) facut cu mke2fs -d si populat din rootfs + date de test.
#   tests/mkext4.sh <imagine> <varianta> <rootfs>   varianta:
#     ext4   blocuri de 4 KiB, extente, 64bit, metadata_csum, flex_bg, jurnal; directoare mari cu htree (1 nivel)
#     ext2   (ext3) blocuri de 1 KiB, harta de blocuri (fara extente), htree pe 2 niveluri, indirectare triplu
#     linear blocuri de 2 KiB, extente, 64bit + meta_bg (56 de grupuri), fara dir_index (directoare liniare)
#   tests/mkext4.sh patch <sursa> <dest> refuse|recover   copie cu superblock-ul stricat: flag INCOMPAT necunoscut /
#                                                         needs_recovery
# Lista asteptata (CRC32 + dimensiune + mod ale fiecarui fisier) ramane in <imagine>.list
set -eu

if [ "$1" = patch ]; then
    python3 - "$2" "$3" "$4" <<'PY'
import shutil, struct, sys
src, dst, what = sys.argv[1:4]
shutil.copyfile(src, dst)
off = 18432 * 512 + 1024 + 0x60
with open(dst, "r+b") as f:
    f.seek(off)
    (inc,) = struct.unpack("<I", f.read(4))
    inc |= 0x100000 if what == "refuse" else 0x4
    f.seek(off)
    f.write(struct.pack("<I", inc))
PY
    exit 0
fi

IMG=${1:?imagine lipsa}
VARIANT=${2:?varianta lipsa}
ROOTFS=${3:?rootfs lipsa}
here=$(dirname "$0")
SECTORS=133120
PSTART=18432
PCOUNT=114688                   # 56 MiB

W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
S=$W/root
mkdir "$S"
cp -a "$ROOTFS"/. "$S"/

python3 - "$S" "$VARIANT" <<'PY'
import os, sys
root, variant = sys.argv[1:3]


def pat(o):
    return (o * 131 + (o >> 9)) & 0xFF


def pat_bytes(start, n):
    return bytes(pat(o) for o in range(start, start + n))


def w(path, data, mode=0o644):
    p = os.path.join(root, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)
    os.chmod(p, mode)


w("VARIANT", variant.encode() + b"\n")
w("data/hello.txt", b"hello from ext4\n")
w("data/empty", b"")
w("data/big.bin", pat_bytes(0, 5 << 20))
for s in (1, 1023, 1024, 1025, 2047, 2048, 2049, 4095, 4096, 4097, 12 * 1024 - 1, 12 * 1024, 12 * 1024 + 1,
          268 * 1024 - 1, 268 * 1024, 268 * 1024 + 1, 1000000):
    w("data/sizes/size_%d" % s, pat_bytes(0, s))
# sparse: insule de 4 KiB la 0, 1, 33, 65 si 66 MiB (65 MiB trece de limita dublei indirectari pe blocuri de 1 KiB)
total = (66 << 20) + 4096
with open(os.path.join(root, "data/sparse.bin"), "wb") as f:
    for o in (0, 1 << 20, 33 << 20, 65 << 20, 66 << 20):
        f.seek(o)
        f.write(pat_bytes(o, 4096))
    f.truncate(total)
os.chmod(os.path.join(root, "data/sparse.bin"), 0o644)
# 1500 insule de 512 octeti la i * 8192: sute de extente, arbore adanc
with open(os.path.join(root, "data/islands.bin"), "wb") as f:
    for i in range(1500):
        o = i * 8192
        f.seek(o)
        f.write(pat_bytes(o, 512))
os.chmod(os.path.join(root, "data/islands.bin"), 0o644)
long_name = "L" * 80
w("data/" + long_name, b"long target\n")
os.symlink("hello.txt", os.path.join(root, "data/link_short"))
os.symlink("../data/" + long_name, os.path.join(root, "data/link_long"))
os.symlink("/data", os.path.join(root, "data/link_dir"))
os.symlink("loop_b", os.path.join(root, "data/loop_a"))
os.symlink("loop_a", os.path.join(root, "data/loop_b"))
os.symlink("nowhere", os.path.join(root, "data/link_dangling"))
w("data/hard_a", b"same inode\n")
os.link(os.path.join(root, "data/hard_a"), os.path.join(root, "data/hard_b"))
os.mkfifo(os.path.join(root, "data/fifo"), 0o640)
os.makedirs(os.path.join(root, "many"))
for i in range(3000):
    w("many/f%05d" % i, b"" if i % 500 else b"x")
for i in range(9000):
    w("huge/huge_entry_%05d_pad" % i, b"")
w("longname/" + "n" * 255, b"255 chars\n")
w("unicode/ünï-țest-日本.txt", "unicode ü\n".encode())
for d, _, _ in os.walk(root):
    if not os.path.islink(d):
        os.chmod(d, 0o755)
PY
chmod 1777 "$S/tmp"
chmod 700 "$S/root"
python3 "$here/ext4_list.py" "$S" > "$IMG.list"

FS=$W/fs.img
case "$VARIANT" in
    ext4)
        mke2fs -q -F -t ext4 -b 4096 -N 20000 -L YKEXT4 -O 64bit,metadata_csum,flex_bg,dir_index,extent \
            -E root_owner=0:0 -d "$S" "$FS" 57344k
        ;;
    ext2)
        mke2fs -q -F -t ext3 -b 1024 -N 20000 -L YKEXT2 -O dir_index,^resize_inode \
            -E root_owner=0:0 -d "$S" "$FS" 57344k
        ;;
    linear)
        mke2fs -q -F -t ext4 -b 2048 -g 512 -N 20000 -L YKLINEAR -O 64bit,extent,meta_bg,^resize_inode,^dir_index,^flex_bg,^metadata_csum \
            -E root_owner=0:0 -d "$S" "$FS" 57344k
        ;;
    *) echo "varianta necunoscuta: $VARIANT"; exit 1 ;;
esac
# Directoarele mai mari de un bloc primesc index htree (mke2fs scrie directoare liniare)
if [ "$VARIANT" != linear ]; then
    rc=0
    e2fsck -fyD "$FS" > "$W/fsck-d.log" 2>&1 || rc=$?
    if [ $rc -ge 4 ]; then cat "$W/fsck-d.log"; echo "e2fsck -D a esuat ($rc)"; exit 1; fi
    for d in /many /huge; do
        debugfs -R "htree_dump $d" "$FS" > "$W/htree.txt" 2>&1
        grep -q "Number of entries" "$W/htree.txt" || { echo "FAIL: $d nu are index htree"; cat "$W/htree.txt" | head; exit 1; }
    done
    echo "htree /huge: $(grep -m1 'Indirect levels' "$W/htree.txt")"
else
    debugfs -R "htree_dump /many" "$FS" 2>&1 | grep -q "Number of entries" && { echo "FAIL: /many ar fi liniar"; exit 1; }
fi
e2fsck -fn "$FS" > "$W/fsck.log" 2>&1 || { cat "$W/fsck.log"; echo "FAIL: e2fsck pe imaginea facuta"; exit 1; }
debugfs -R "dump_extents /data/islands.bin" "$FS" 2>/dev/null | awk '/^ *[0-9]+\/ *[0-9]+/ {l=$1} END {if (l) print "extente islands.bin, ultimul nivel: " l}' || true

rm -f "$IMG"
truncate -s $((SECTORS * 512)) "$IMG"
python3 - "$IMG" "$PSTART" "$PCOUNT" <<'PY'
import struct, sys
img, pstart, pcount = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
with open(img, "r+b") as f:
    mbr = bytearray(512)
    mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\x00\x00\x00", 0x83, b"\x00\x00\x00", 2048, 16384)
    mbr[462:478] = struct.pack("<B3sB3sII", 0, b"\x00\x00\x00", 0x83, b"\x00\x00\x00", pstart, pcount)
    mbr[510:512] = b"\x55\xAA"
    f.write(mbr)
PY
dd if="$FS" of="$IMG" bs=512 seek=$PSTART conv=notrunc 2> /dev/null

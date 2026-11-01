#!/bin/sh
# Dupa un boot al test=fs: verifica de pe gazda imaginea facuta de tests/mkdisk.sh.
#   fsck.fat -n pe sda1 si e2fsck -fn pe sda2 (fara needs_recovery), apoi extrage ambele sisteme de fisiere (mtools /
#   debugfs) si compara SHA-256 cu manifestele: MANIFEST.SHA (de la mkdisk.sh) si, dupa boot-ul 1, NEW.SHA (scris de
#   kernel) + continutul recalculat aici cu generatorul din kernel/tests/test_fs.cpp; nu are voie sa existe alte fisiere
# Uz: tests/check_fs.sh <imagine> <faza: 1|2> <prefix mkdisk>   (faza 1 = dupa boot-ul care scrie, 2 = dupa regasire;
# prefix = cel dat lui tests/mkdisk.sh, de unde vin copiile manifestelor)
set -eu

IMG=${1:?imagine lipsa}
PHASE=${2:?faza lipsa}
PREFIX=${3:?prefix lipsa}
P1_START=2048
P1_COUNT=98304
P2_START=$((P1_START + P1_COUNT))
P2_COUNT=131072
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export MTOOLS_SKIP_CHECK=1

fail(){
    echo "FAIL (check_fs faza $PHASE): $*"
    exit 1
}

dd if="$IMG" of="$W/p1.img" bs=512 skip=$P1_START count=$P1_COUNT status=none
dd if="$IMG" of="$W/p2.img" bs=512 skip=$P2_START count=$P2_COUNT status=none

fsck.fat -n -v "$W/p1.img" > "$W/fsck.fat.log" 2>&1 || { cat "$W/fsck.fat.log"; fail "fsck.fat -n a gasit erori"; }
e2fsck -fn "$W/p2.img" > "$W/e2fsck.log" 2>&1 || { cat "$W/e2fsck.log"; fail "e2fsck -fn a gasit erori"; }
if dumpe2fs -h "$W/p2.img" 2> /dev/null | grep -q '^Filesystem features:.*needs_recovery'; then
    fail "ext4: needs_recovery ramas dupa umount"
fi
echo "fsck.fat -n curat; e2fsck -fn: $(tail -1 "$W/e2fsck.log")"

mkdir "$W/fat32" "$W/ext4"
mcopy -s -n -i "$W/p1.img" :: "$W/fat32" || fail "mcopy"
debugfs -R "rdump / $W/ext4" "$W/p2.img" > /dev/null 2>&1 || fail "debugfs rdump"
rm -rf "$W/ext4/lost+found"
cmp "$W/fat32/MANIFEST.SHA" "$PREFIX.fat32.sha" || fail "MANIFEST.SHA de pe FAT32 difera de al mkdisk.sh"
cmp "$W/ext4/MANIFEST.SHA" "$PREFIX.ext4.sha" || fail "MANIFEST.SHA de pe ext4 difera de al mkdisk.sh"

python3 - "$W" "$PHASE" <<'PY' || exit 1
import hashlib, os, sys

w, phase = sys.argv[1], int(sys.argv[2])
# oglinda lui NEWS din kernel/tests/test_fs.cpp: (cale, seed de baza, dimensiune)
NEWS = [("new/kernel.txt", 11, 30), ("new/blob.bin", 12, 700001), ("new/sub/exact4k.bin", 13, 4096),
        ("new/Kernel Written Long Name.txt", 14, 1), ("new/renamed.bin", 15, 70000)]
M = 0xFFFFFFFF


def gen(seed, n):
    out = bytearray(n)
    for off in range(n):
        x = (off * 0x9E3779B1 + seed * 0x85EBCA6B) & M
        x ^= x >> 15
        x = (x * 0x2C1B3C6D) & M
        x ^= x >> 12
        x = (x * 0x297A2D39) & M
        x ^= x >> 15
        out[off] = (x >> 8) & 0xFF
    return bytes(out)


def walk(root):
    res = {}
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            res[os.path.relpath(p, root)] = p
    return res


def read_manifest(path):
    ents = {}
    for line in open(path).read().splitlines():
        h, size, rel = line.split(" ", 2)
        ents[rel] = (h, int(size))
    return ents


bad = 0
for idx, fs in enumerate(("fat32", "ext4")):
    root = os.path.join(w, fs)
    found = walk(root)
    # directoarele gole ar fi o scapare
    expected = {}
    orig = read_manifest(os.path.join(root, "MANIFEST.SHA"))
    # manifestul de pe disc trebuie sa fie cel facut de mkdisk.sh (copia ramane langa imagine)
    expected.update(orig)
    expected["MANIFEST.SHA"] = None
    if phase >= 1 and os.path.exists(os.path.join(root, "NEW.SHA")):
        newm = read_manifest(os.path.join(root, "NEW.SHA"))
        if sorted(newm) != sorted(r for r, _, _ in NEWS):
            print("FAIL %s: NEW.SHA listeaza %s" % (fs, sorted(newm))); bad = 1
        for rel, base, size in NEWS:
            data = gen(base + 100 * idx, size)
            want = hashlib.sha256(data).hexdigest()
            if newm.get(rel) != (want, size):
                print("FAIL %s: %s in NEW.SHA = %s, recalculat %s" % (fs, rel, newm.get(rel), (want, size))); bad = 1
        expected.update(newm)
        expected["NEW.SHA"] = None
    elif phase >= 1:
        print("FAIL %s: lipseste NEW.SHA" % fs); bad = 1
    # FAT: numele pot diferi ca litere mari / mici doar daca mtools le-a scurtat; comparam exact
    for rel in sorted(set(expected) | set(found)):
        if rel not in found:
            print("FAIL %s: lipseste %s" % (fs, rel)); bad = 1
        elif rel not in expected:
            print("FAIL %s: fisier in plus %s" % (fs, rel)); bad = 1
        elif expected[rel] is not None:
            data = open(found[rel], "rb").read()
            h = hashlib.sha256(data).hexdigest()
            if (h, len(data)) != expected[rel]:
                print("FAIL %s: %s sha256/dimensiune %s %d, asteptat %s" % (fs, rel, h, len(data), expected[rel])); bad = 1
    print("%s: %d fisiere comparate cu manifestele" % (fs, len(expected)))
if bad:
    sys.exit(1)
PY
echo "PASS (check_fs faza $PHASE: fsck.fat -n, e2fsck -fn, SHA-256 pe host)"

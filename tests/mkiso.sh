#!/bin/sh
# Imagine ISO 9660 cu Rock Ridge pentru test=iso9660 (make test-iso): nume lungi si cu majuscule, symlink-uri, directoare
# adanci (relocate cu CL/RE), director de 300 de intrari, fisier de 300000 de octeti, nume UTF-8, bit de executie.
# Uz: tests/mkiso.sh <imagine.iso> [director_de_lucru]
set -eu

IMG=${1:?imagine lipsa}
W=${2:-$(mktemp -d)}
T=$W/tree

rm -rf "$T"
mkdir -p "$T"
python3 - "$T" <<'PY'
import os, sys
t = sys.argv[1]

def put(path, data):
    p = os.path.join(t, path)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "wb") as f:
        f.write(data)

put("hello.txt", b"hello from iso\n")
put("Long Mixed Case Name.TXT", b"long name\n")
put("empty", b"")
os.makedirs(os.path.join(t, "emptydir"))
put("run.sh", b"#!/bin/sh\n")
os.chmod(os.path.join(t, "run.sh"), 0o755)
put("big.bin", bytes((i * 131 + (i >> 9)) & 0xFF for i in range(300000)))
put("sub/deeper/nested.txt", b"nested file\n")
put("deep/" + "/".join("d%d" % i for i in range(1, 13)) + "/leaf.txt", b"leaf\n")
for i in range(300):
    put("many/f%04d.txt" % i, b"file %04d\n" % i)
put("Țară.txt", "unicode über\n".encode())
put("y" * 200 + ".txt", b"long200\n")
os.symlink("hello.txt", os.path.join(t, "link"))
os.symlink("/abs/target", os.path.join(t, "abslink"))
os.symlink("../hello.txt", os.path.join(t, "sub/uplink"))
os.symlink("sub/deeper", os.path.join(t, "deeplink"))
PY
rm -f "$IMG"
xorriso -as mkisofs -r -V YOKAITEST -rr_reloc_dir rr_moved -o "$IMG" "$T" >/dev/null 2>&1

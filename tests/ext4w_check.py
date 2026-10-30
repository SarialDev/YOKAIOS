#!/usr/bin/env python3
# Compara liniile "<TAG> ..." tiparite de test_ext4w cu partitia ext4 citita de debugfs (e2fsprogs):
#   D <cale> <n>                  director cu n intrari, fiecare are propria linie
#   F <crc32> <dimensiune> <cale> fisier (CRC32 zlib)
#   L <cale> -> <tinta>           symlink
#   M <cale> <n> <suma> <xor>     director mare: numarul de nume si suma / xor-ul CRC32-urilor lor
#   A <cale>                      nu exista
# Uz: tests/ext4w_check.py <partitie.img> <log> <TAG>
import os
import subprocess
import sys
import tempfile
import zlib

img, log, tag = sys.argv[1:4]


def debugfs(cmds):
    """Ruleaza comenzile intr-un singur debugfs; intoarce iesirea fiecarei comenzi."""
    with tempfile.NamedTemporaryFile("w", suffix=".cmd", delete=False) as f:
        f.write("\n".join(cmds) + "\n")
        name = f.name
    try:
        out = subprocess.run(["debugfs", "-f", name, img], capture_output=True, text=True,
                             errors="surrogateescape").stdout
    finally:
        os.unlink(name)
    res = []
    for line in out.splitlines():
        if line.startswith("debugfs: "):
            res.append([])
        elif res:
            res[-1].append(line)
    if len(res) != len(cmds):
        sys.exit("FAIL: debugfs a intors %d raspunsuri pentru %d comenzi" % (len(res), len(cmds)))
    return res


def names(lines):
    out = []
    for l in lines:
        if not l.startswith("/"):
            continue
        parts = l.split("/")
        # /ino/mode/uid/gid/nume/dimensiune/
        n = "/".join(parts[5:-2])
        # inode 0: intrare stearsa (prima dintr-un bloc nu se poate lipi de cea dinainte)
        if n not in (".", "..") and parts[1] != "0":
            out.append(n)
    return out


want = []
for line in open(log, errors="replace"):
    line = line.rstrip("\n")
    if line.startswith(tag + " "):
        want.append(line[len(tag) + 1:])
if not want:
    sys.exit("FAIL: nicio linie %s in %s" % (tag, log))

dirs, files, links, bigs, absent = {}, {}, {}, {}, []
for w in want:
    kind, rest = w.split(" ", 1)
    if kind == "D":
        p, n = rest.rsplit(" ", 1)
        dirs[p] = int(n)
    elif kind == "F":
        crc, size, p = rest.split(" ", 2)
        files[p] = (int(crc, 16), int(size))
    elif kind == "L":
        p, t = rest.split(" -> ", 1)
        links[p] = t
    elif kind == "M":
        p, n, s, x = rest.split(" ")
        bigs[p] = (int(n), int(s, 16), int(x, 16))
    elif kind == "A":
        absent.append(rest)
    else:
        sys.exit("FAIL: linie necunoscuta: %s %s" % (tag, w))

known = set(dirs) | set(files) | set(links) | set(bigs)
bad = []
# Directoare: exact intrarile tiparite
dl = sorted(dirs) + sorted(bigs)
for p, out in zip(dl, debugfs(["ls -p \"%s\"" % p for p in dl])):
    ns = names(out)
    if p in dirs:
        if len(ns) != dirs[p]:
            bad.append("%s: %d intrari pe disc, kernelul a vazut %d" % (p, len(ns), dirs[p]))
        for n in ns:
            c = (p.rstrip("/") + "/" + n)
            if c not in known:
                bad.append("%s exista pe disc dar lipseste din liniile kernelului" % c)
    else:
        n, s, x = bigs[p]
        cs = [zlib.crc32(n_.encode()) for n_ in ns]
        sm, xr = sum(cs) & 0xFFFFFFFF, 0
        for c in cs:
            xr ^= c
        if (len(ns), sm, xr) != (n, s, x):
            bad.append("%s: %d nume (%08x %08x), kernelul: %d (%08x %08x)" % (p, len(ns), sm, xr, n, s, x))
# Fisiere: continut
with tempfile.TemporaryDirectory() as td:
    fl = sorted(files)
    debugfs(["dump \"%s\" \"%s/%d\"" % (p, td, i) for i, p in enumerate(fl)])
    for i, p in enumerate(fl):
        fp = "%s/%d" % (td, i)
        if not os.path.exists(fp):
            bad.append("%s: lipseste pe disc" % p)
            continue
        data = open(fp, "rb").read()
        crc, size = files[p]
        if len(data) != size or zlib.crc32(data) != crc:
            bad.append("%s: %d octeti crc %08x pe disc, kernelul: %d %08x" % (p, len(data), zlib.crc32(data), size,
                                                                            crc))
    ll = sorted(links)
    stats = debugfs(["stat \"%s\"" % p for p in ll])
    slow = []
    for p, out in zip(ll, stats):
        fast = [l for l in out if "Fast link dest:" in l]
        if fast:
            t = fast[0].split("Fast link dest: ", 1)[1].strip().strip('"')
            if t != links[p]:
                bad.append("%s -> %s pe disc, kernelul: %s" % (p, t, links[p]))
        else:
            slow.append(p)
    debugfs(["dump \"%s\" \"%s/l%d\"" % (p, td, i) for i, p in enumerate(slow)])
    for i, p in enumerate(slow):
        fp = "%s/l%d" % (td, i)
        t = open(fp, "rb").read().decode(errors="replace") if os.path.exists(fp) else None
        if t != links[p]:
            bad.append("%s -> %r pe disc, kernelul: %s" % (p, t, links[p]))
for p in absent:
    r = subprocess.run(["debugfs", "-R", "stat \"%s\"" % p, img], capture_output=True, text=True)
    if "not found" not in r.stderr + r.stdout:
        bad.append("%s exista pe disc, kernelul n-a gasit-o" % p)

if bad:
    print("FAIL: %s difera de disc (%s):" % (tag, img))
    for b in bad[:20]:
        print("  " + b)
    sys.exit(1)
print("%s identic cu debugfs: %d directoare, %d fisiere, %d symlink-uri, %d directoare mari, %d absente"
      % (tag, len(dirs), len(files), len(links), len(bigs), len(absent)))

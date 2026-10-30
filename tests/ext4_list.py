#!/usr/bin/env python3
"""Lista asteptata pentru test=ext4: uz: ext4_list.py <director_sursa>
Aceleasi linii "EXT4 ..." pe care le tipareste kernelul dupa ce parcurge arborele montat (tests/test_ext4.cpp):
  EXT4 F <crc32> <dimensiune> <mod> <cale>   fisier (crc32 ca zlib.crc32)
  EXT4 D <mod> <cale>                        director
  EXT4 L <cale> -> <tinta>                   symlink
  EXT4 P <mod> <cale>                        fifo
mod = permisiuni in hex pe 4 cifre (&07777). Plus /lost+found, creat de mke2fs.
"""
import os, stat, sys, zlib


def walk(root, rel, out):
    with os.scandir(os.path.join(root, rel) if rel else root) as it:
        entries = sorted(it, key=lambda e: e.name)
    for e in entries:
        p = os.path.join(rel, e.name) if rel else e.name
        st = e.stat(follow_symlinks=False)
        kpath = b"/" + os.fsencode(p)
        mode = st.st_mode
        if stat.S_ISDIR(mode):
            out.append(b"EXT4 D %04x " % (mode & 0o7777) + kpath)
            walk(root, p, out)
        elif stat.S_ISREG(mode):
            crc = 0
            with open(os.path.join(root, p), "rb") as f:
                while True:
                    b = f.read(1 << 20)
                    if not b:
                        break
                    crc = zlib.crc32(b, crc)
            out.append(b"EXT4 F %08x %d %04x " % (crc, st.st_size, mode & 0o7777) + kpath)
        elif stat.S_ISLNK(mode):
            out.append(b"EXT4 L " + kpath + b" -> " + os.fsencode(os.readlink(os.path.join(root, p))))
        elif stat.S_ISFIFO(mode):
            out.append(b"EXT4 P %04x " % (mode & 0o7777) + kpath)
        else:
            raise SystemExit("tip neasteptat: " + p)


def main():
    out = [b"EXT4 D %04x /lost+found" % 0o700]
    walk(sys.argv[1], "", out)
    sys.stdout.buffer.write(b"\n".join(out) + b"\n")


main()

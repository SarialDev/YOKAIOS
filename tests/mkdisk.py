#!/usr/bin/env python3
"""Imagine de disc de 32 MiB pentru test=ahci: uz: mkdisk.py gpt|mbr <fisier>

Sectorul 0 = "YOKAI-AHCI-SECTOR0" + (i*7+3) pana la 446, tabela de partitii, 55AA.
Prima sectoare a fiecarei partitii = "YKPART" + cifra numarului ei (sda<cifra>).
  gpt: sda1 2048+16384, sda2 18432+32768
  mbr: sda1 2048+16384 (0x83), sda2 18432+16384 (0x0C), extinsa 34816+20480 cu logice sda5 34818+10238, sda6 45058+10238
"""
import struct, sys, zlib

SECTORS = 65536
SS = 512


def part_marker(n):
    return b"YKPART" + bytes([ord("0") + n]) + bytes((i * 5 + n) & 0xFF for i in range(SS - 7))


def sector0():
    s = bytearray(SS)
    s[0:18] = b"YOKAI-AHCI-SECTOR0"
    for i in range(18, 446):
        s[i] = (i * 7 + 3) & 0xFF
    s[510:512] = b"\x55\xAA"
    return s


def mbr_entry(s, slot, ptype, start, count):
    o = 446 + 16 * slot
    s[o + 4] = ptype
    s[o + 8:o + 16] = struct.pack("<II", start, count)


def write(img, lba, data):
    img.seek(lba * SS)
    img.write(data)


def make_mbr(img):
    s0 = sector0()
    mbr_entry(s0, 0, 0x83, 2048, 16384)
    mbr_entry(s0, 1, 0x0C, 18432, 16384)
    mbr_entry(s0, 2, 0x05, 34816, 20480)
    write(img, 0, s0)
    write(img, 2048, part_marker(1))
    write(img, 18432, part_marker(2))
    ext = 34816
    for lba, n, nxt in ((ext, 5, ext + 10240), (ext + 10240, 6, None)):
        e = bytearray(SS)
        mbr_entry(e, 0, 0x83, 2, 10238)
        if nxt:
            mbr_entry(e, 1, 0x05, nxt - ext, 10240)
        e[510:512] = b"\x55\xAA"
        write(img, lba, e)
        write(img, lba + 2, part_marker(n))


def gpt_header(my, alt, table_lba, table_crc):
    h = bytearray(92)
    h[0:8] = b"EFI PART"
    struct.pack_into("<IIIIQQQQ", h, 8, 0x00010000, 92, 0, 0, my, alt, 34, SECTORS - 34)
    h[56:72] = bytes(range(1, 17))
    struct.pack_into("<QIII", h, 72, table_lba, 128, 128, table_crc)
    struct.pack_into("<I", h, 16, zlib.crc32(bytes(h)) & 0xFFFFFFFF)
    return bytes(h).ljust(SS, b"\0")


def make_gpt(img):
    s0 = sector0()
    mbr_entry(s0, 0, 0xEE, 1, SECTORS - 1)
    write(img, 0, s0)
    linux = bytes([0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47, 0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4])
    ents = bytearray(128 * 128)
    for i, (first, last) in enumerate(((2048, 2048 + 16384 - 1), (18432, 18432 + 32768 - 1))):
        e = bytearray(128)
        e[0:16] = linux
        e[16:32] = bytes([0x10 + i]) * 16
        struct.pack_into("<QQQ", e, 32, first, last, 0)
        e[56:56 + 12] = ("YOKAI%d" % (i + 1)).encode("utf-16-le")
        ents[128 * i:128 * i + 128] = e
        write(img, first, part_marker(i + 1))
    crc = zlib.crc32(bytes(ents)) & 0xFFFFFFFF
    write(img, 2, ents)
    write(img, SECTORS - 33, ents)
    write(img, 1, gpt_header(1, SECTORS - 1, 2, crc))
    write(img, SECTORS - 1, gpt_header(SECTORS - 1, 1, SECTORS - 33, crc))


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("gpt", "mbr"):
        sys.exit(__doc__)
    with open(sys.argv[2], "wb") as img:
        img.truncate(SECTORS * SS)
        (make_gpt if sys.argv[1] == "gpt" else make_mbr)(img)


main()

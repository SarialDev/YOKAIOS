#!/usr/bin/env python3
"""Tabelul de relocari KASLR pentru kernel.bin (link cu --emit-relocs).

  kaslr_relocs.py extract <kernel.elf> <out.bin>   scrie tabelul (format citit de boot/kaslr.c)
  kaslr_relocs.py verify  <kernel.elf>             tabelul din sectiunea .kaslr_relocs = cel extras din acelasi ELF

Se corecteaza valorile absolute legate de imaginea kernelului: R_X86_64_64 daca valoarea e in KERNEL_VMA + [0, 1 GiB),
R_X86_64_32S daca simbolul nu e absolut (orice "simbol + constanta" se muta cu kernelul). Relocarile relative la PC nu se schimba (codul si tinta se muta impreuna), iar
R_X86_64_32 / valorile mici sunt adrese fizice (PHYS(x) din boot.S) si raman pe loc.
"""
import struct
import sys

KERNEL_VMA = 0xFFFFFFFF80000000
WINDOW = 1 << 30
MAGIC = 0x314C524B
SHT_SYMTAB, SHT_RELA, SHT_NOBITS = 2, 4, 8
SHF_ALLOC = 2
SHN_ABS = 0xFFF1
R_NONE, R_64, R_PC32, R_GOT32, R_PLT32 = 0, 1, 2, 3, 4
R_32, R_32S, R_16, R_PC16, R_8, R_PC8, R_PC64 = 10, 11, 12, 13, 14, 15, 24
PC_RELATIVE = {R_PC32, R_PLT32, R_PC16, R_PC8, R_PC64}


def die(msg):
    sys.stderr.write("kaslr_relocs: %s\n" % msg)
    sys.exit(1)


class Elf:
    def __init__(self, path):
        self.data = open(path, "rb").read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            die("%s nu e ELF64 little-endian" % path)
        shoff, = struct.unpack_from("<Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
        self.sec = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(
                "<IIQQQQIIQQ", d, shoff + i * shentsize)
            self.sec.append(dict(name=name, type=typ, flags=flags, addr=addr, off=off, size=size,
                                 link=link, info=info, entsize=entsize))
        strtab = self.sec[shstrndx]
        for s in self.sec:
            end = d.index(b"\0", strtab["off"] + s["name"])
            s["name"] = d[strtab["off"] + s["name"]:end].decode()

    def section(self, name):
        for s in self.sec:
            if s["name"] == name:
                return s
        return None

    def read(self, vaddr, size):
        for s in self.sec:
            if s["flags"] & SHF_ALLOC and s["type"] != SHT_NOBITS and s["addr"] <= vaddr and vaddr + size <= s["addr"] + s["size"]:
                o = s["off"] + vaddr - s["addr"]
                return self.data[o:o + size]
        die("adresa %x nu e intr-o sectiune cu continut" % vaddr)


def in_kernel(v):
    return KERNEL_VMA <= v < KERNEL_VMA + WINDOW


def extract(elf):
    r64, r32 = set(), set()
    for rs in elf.sec:
        if rs["type"] != SHT_RELA:
            continue
        tgt = elf.sec[rs["info"]]
        if not tgt["flags"] & SHF_ALLOC or tgt["name"] == ".kaslr_relocs":
            continue
        symtab = elf.sec[rs["link"]]
        if symtab["type"] != SHT_SYMTAB:
            die("%s: link nu e .symtab" % rs["name"])
        for i in range(rs["size"] // 24):
            off, info, addend = struct.unpack_from("<QQq", elf.data, rs["off"] + i * 24)
            typ, symi = info & 0xFFFFFFFF, info >> 32
            if off < tgt["addr"]:               # relocari relative la sectiune (nu ar trebui in ET_EXEC)
                off += tgt["addr"]
            shndx, = struct.unpack_from("<H", elf.data, symtab["off"] + symi * 24 + 6)
            if typ == R_NONE or typ in PC_RELATIVE:
                continue
            if typ == R_64:
                v, = struct.unpack("<Q", elf.read(off, 8))
                if shndx != SHN_ABS and in_kernel(v):
                    r64.add(off)
            elif typ == R_32S:
                # dupa simbol, nu dupa valoare: compilatorul plieaza "simbol + constanta" (ex. _kernel_start + 0x7ff00000
                # dintr-o comparatie) intr-un imediat care nu mai arata ca o adresa din kernel, dar se muta cu el
                if shndx != SHN_ABS:
                    r32.add(off)
            elif typ in (R_32, R_16, R_8):
                pass                            # zero-extinse: nu pot tine o adresa din higher-half
            else:
                die("%s: tip de relocare %d nesuportat la %x" % (rs["name"], typ, off))
    for o in list(r64) + list(r32):
        if not in_kernel(o):
            die("loc de relocare in afara imaginii: %x" % o)
    r64, r32 = sorted(r64), sorted(r32)
    out = struct.pack("<IIII", MAGIC, len(r64), len(r32), 0)
    out += b"".join(struct.pack("<I", o - KERNEL_VMA) for o in r64 + r32)
    return out, len(r64), len(r32)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "extract":
        out, n64, n32 = extract(Elf(sys.argv[2]))
        open(sys.argv[3], "wb").write(out)
        print("kaslr_relocs: %d x 64 bit, %d x 32S" % (n64, n32))
    elif len(sys.argv) == 3 and sys.argv[1] == "verify":
        elf = Elf(sys.argv[2])
        out, _, _ = extract(elf)
        s = elf.section(".kaslr_relocs")
        if not s or elf.data[s["off"]:s["off"] + s["size"]] != out:
            die("%s: .kaslr_relocs nu corespunde imaginii (link-ul s-a schimbat intre pasi?)" % sys.argv[2])
    else:
        die("uz: kaslr_relocs.py extract <elf> <out.bin> | verify <elf>")


if __name__ == "__main__":
    main()

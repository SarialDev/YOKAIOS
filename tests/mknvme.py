#!/usr/bin/env python3
"""Namespace 2 pentru test=nvme: uz: mknvme.py <fisier>   (4 MiB, sectoare de 4096 B)

Sectorul 0 = "YOKAI-NVME-NS2" + (i*3+1) pana la 4096; restul zero.
"""
import sys

SS = 4096
BLOCKS = 1024

s = bytearray(SS)
s[0:14] = b"YOKAI-NVME-NS2"
for i in range(14, SS):
    s[i] = (i * 3 + 1) & 0xFF
with open(sys.argv[1], "wb") as f:
    f.write(s)
    f.write(bytes(SS * (BLOCKS - 1)))

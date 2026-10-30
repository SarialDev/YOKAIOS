#pragma once

// FAT32 pe disc SATA (make test-fat32): imagine facuta cu mkfs.fat si mtools pe host (tests/mkfat32.sh), montata
// pe /media la boot; citire (BPB, nume lungi, lanturi), scriere, extindere, creare / stergere / redenumire de fisiere
// si directoare, remontare. Dupa test, gazda verifica imaginea cu fsck.fat. cmdline "test=fat32"
void test_fat32_run();

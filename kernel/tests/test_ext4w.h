#pragma once

// ext4 cu scriere si jurnal (make test-ext4w, orchestrat de tests/check_ext4w.sh). Radacina ramane initrd-ul; sda2
// (ext4 facut de tests/mkext4.sh) se monteaza cu scriere pe /mnt. Cmdline "test=ext4w"; ce face fiecare boot scrie
// gazda in primul sector al lui sda1 (text, terminat cu 0 sau newline), de ex. "ext4w mode=write crash=3":
//   mode=write crash=N    starea S1 (comisa), apoi S2 intr-o singura tranzactie si o oprire brusca a QEMU la pasul N
//                         al commit-ului: 1 = jurnal scris fara blocul de commit, 2 = dupa commit, 3 = checkpoint la
//                         jumatate, 4 = dupa checkpoint (jurnal negolit), 5 = S2 necomisa, 0 = fara cadere (umount)
//   mode=verify crash=N   montarea reia jurnalul; arborele trebuie sa fie exact S1 (N = 1, 5) sau S2; tipareste
//                         liniile "EXT4S" (starea gasita), mai scrie, tipareste "EXT4W" si demonteaza curat
//   mode=stress seed=S    scrieri la nesfarsit (commit=1) pana cand gazda omoara QEMU cu SIGKILL
//   mode=stresscheck      dupa kill: fiecare fisier din /st e consistent (continut generat din id-ul din antet)
//   mode=replay           jurnal scris de debugfs pe gazda: 2 tranzactii reluate, revoke respectat; apoi scrieri si
//                         cadere dupa commit (etapa 2)
//   mode=replay2          reia jurnalul scris de noi la boot-ul replay (gazda l-a reluat si cu e2fsck pe o copie)
// Liniile "EXT4S"/"EXT4W" (D/F/L/M/A, cai fata de radacina sistemului de fisiere) sunt verificate de
// tests/ext4w_check.py cu debugfs, pe imaginea finala si pe o copie reluata de e2fsprogs.
void test_ext4w_run();

#pragma once

// Integrare stocare (make test-fs): un disc SATA GPT cu sda1 = FAT32 (montat de kernel pe /media/disk) si sda2 =
// ext4 (montat pe /mnt), facut de tests/mkdisk.sh. Fiecare sistem de fisiere are un manifest MANIFEST.SHA
// ("<sha256> <dimensiune> <cale>" pe linie) cu fisiere de continut cunoscut.
//   boot 1 (nu exista NEW.SHA): verifica SHA-256 al fiecarui fisier din manifest, scrie fisiere noi pe ambele sisteme
//          (creare, mkdir, scriere in bucati neregulate, rename, unlink), le descrie in NEW.SHA, demonteaza curat
//   boot 2 (exista NEW.SHA): verifica din nou manifestul initial si pe NEW.SHA (fisierele noi trebuie regasite, cele
//          sterse / redenumite sa lipseasca), ext4 montat "ro", demonteaza
// Gazda ruleaza fsck.fat -n si e2fsck -fn dupa fiecare boot (tests/check_fs.sh). cmdline "test=fs"
void test_fs_run();

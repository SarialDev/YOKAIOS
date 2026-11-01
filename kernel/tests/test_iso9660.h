#pragma once

// ISO 9660 + Rock Ridge (make test-iso): CD-ul de boot (sr0 pe AHCI) montat automat pe /media/cdrom, apoi o imagine
// facuta cu xorriso (tests/mkiso.sh, sr1) montata pe /mnt: nume lungi si cu majuscule, symlink-uri, directoare relocate
// (CL/RE), 300 de intrari, fisier de 300000 de octeti, UTF-8, moduri. cmdline "test=iso9660"
void test_iso9660_run();

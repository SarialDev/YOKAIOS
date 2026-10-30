#pragma once

// VFS peste un sistem de fisiere fals in memorie (memfs, doar pentru teste): rezolvare de cai, symlink-uri,
// mount/umount, operatii pe fisiere si directoare, getdents, FdTable; cmdline "test=vfs"
void test_vfs_run();

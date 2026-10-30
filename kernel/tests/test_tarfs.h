#pragma once

// initrd.tar montat pe "/" cu tarfs: layout-ul din blueprint, citire, symlink-uri, read-only, montari multiple,
// execve din VFS; cmdline "test=tarfs"
void test_tarfs_run();

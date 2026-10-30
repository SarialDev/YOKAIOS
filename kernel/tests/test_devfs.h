#pragma once

// devfs pe /dev: null, zero, random, console, ttyS0, fb0 si dispozitivele bloc din HAL (sda, nvme0n1) care apar
// singure la inregistrare; cmdline "test=devfs"
void test_devfs_run();

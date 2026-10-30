#pragma once
#include "types.h"

// CRC32C (Castagnoli) "brut", ca in kernelul Linux: fara inversare la intrare / iesire (seed-ul si rezultatul
// se dau asa cum le foloseste ext4 / jbd2, ex. crc32c(~0u, uuid, 16))
uint32_t crc32c(uint32_t crc, const void* data, uint64_t len);
// CRC16 (polinom 0xA001, reflectat) ca in ext2fs_crc16, folosit de uninit_bg / gdt_csum
uint16_t crc16(uint16_t crc, const void* data, uint64_t len);

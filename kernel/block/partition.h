#pragma once
#include "types.h"
#include "hal/block.h"

// Tabele de partitii MBR (primare 1-4, logice 5+ prin lantul EBR) si GPT (CRC32 verificat, antet de rezerva).
// O tabela MBR cu o intrare protectiva 0xEE se citeste ca GPT.

enum class PartScheme : uint8_t { None, Mbr, Gpt };

struct PartInfo {
    uint8_t number;             // 1.. (numarul din sdaN)
    uint8_t type;               // tip MBR, 0 la GPT
    bool gpt;
    uint64_t start;             // in sectoare ale discului
    uint64_t count;
    uint8_t type_guid[16];      // doar GPT
    uint8_t part_guid[16];
};

const int PART_MAX = 32;

uint32_t crc32(const void* data, size_t len);

// Citeste tabela de pe dev; intoarce numarul de partitii valide (0 daca nu exista tabela). scheme poate fi nullptr
int partition_scan(BlockDevice* dev, PartInfo* out, int max, PartScheme* scheme);

class PartitionDevice final : public BlockDevice {
public:
    PartitionDevice(BlockDevice* parent_dev, const PartInfo& pi, const char* base_name);
    uint32_t block_size() const override { return parent->block_size(); }
    uint64_t block_count() const override { return count; }
    int read(uint64_t lba, uint32_t n, void* buf) override;
    int write(uint64_t lba, uint32_t n, const void* buf) override;
    int flush() override { return parent->flush(); }
    BlockDevice* parent;
    uint64_t start;
    uint64_t count;
    PartInfo info;
    char name[16];              // "sda1", "nvme0n1p1"
};

// Scaneaza disk si inregistreaza fiecare partitie in HAL (hal_register) sub numele base+numar
// (cu 'p' intre ele cand base se termina in cifra). Intoarce numarul de partitii inregistrate
int partition_register(BlockDevice* disk, const char* base_name);
// Partitia care infasoara dev, sau nullptr daca dev nu e o partitie creata aici
const PartitionDevice* partition_find(const BlockDevice* dev);

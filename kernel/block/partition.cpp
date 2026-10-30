#include "block/partition.h"
#include "hal/hal.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace {

const int MAX_PARTS_REGISTERED = 64;
PartitionDevice* registered[MAX_PARTS_REGISTERED];
int registered_count;

uint32_t le32(const uint8_t* p){
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t le64(const uint8_t* p){
    return le32(p) | ((uint64_t)le32(p + 4) << 32);
}

bool valid_bs(uint32_t bs){
    return bs >= 512 && bs <= 4096 && (bs & (bs - 1)) == 0;
}

bool is_extended(uint8_t t){
    return t == 0x05 || t == 0x0F || t == 0x85;
}

struct Scan {
    BlockDevice* dev;
    PartInfo* out;
    int max;
    int n;

    bool add_mbr(uint8_t number, uint8_t type, uint64_t start, uint64_t count){
        if(!type || !count || start + count < start || start + count > dev->block_count()){
            if(type && count){
                kprintf("partition: partitia %d iese din disc, ignorata\n", number);
            }
            return false;
        }
        if(n >= max){
            return false;
        }
        PartInfo& p = out[n++];
        memset(&p, 0, sizeof(p));
        p.number = number;
        p.type = type;
        p.start = start;
        p.count = count;
        return true;
    }
};

bool read_sectors(BlockDevice* dev, uint64_t lba, uint32_t count, void* buf){
    return lba + count <= dev->block_count() && dev->read(lba, count, buf) == 0;
}

// Antet GPT la lba; intoarce numarul de intrari adaugate sau -1 daca antetul/tabela nu sunt valide
int scan_gpt_at(Scan& s, uint64_t lba){
    BlockDevice* dev = s.dev;
    uint32_t bs = dev->block_size();
    uint8_t* hdr = (uint8_t*)kmalloc(bs);
    if(!hdr){
        return -1;
    }
    int result = -1;
    uint8_t* ents = nullptr;
    do {
        if(!read_sectors(dev, lba, 1, hdr) || memcmp(hdr, "EFI PART", 8) != 0){
            break;
        }
        uint32_t hsize = le32(hdr + 12);
        if(hsize < 92 || hsize > bs){
            break;
        }
        uint32_t crc = le32(hdr + 16);
        memset(hdr + 16, 0, 4);
        if(crc32(hdr, hsize) != crc){
            break;
        }
        uint64_t table_lba = le64(hdr + 72);
        uint32_t num = le32(hdr + 80);
        uint32_t esz = le32(hdr + 84);
        uint32_t table_crc = le32(hdr + 88);
        if(num == 0 || num > 128 || esz < 128 || esz > 512 || (esz & 7)){
            break;
        }
        uint32_t bytes = num * esz;
        uint32_t sectors = (bytes + bs - 1) / bs;
        ents = (uint8_t*)kmalloc((size_t)sectors * bs);
        if(!ents || !read_sectors(dev, table_lba, sectors, ents) || crc32(ents, bytes) != table_crc){
            break;
        }
        int before = s.n;
        for(uint32_t i = 0; i < num && s.n < s.max; i++){
            const uint8_t* e = ents + (size_t)i * esz;
            bool used = false;
            for(int b = 0; b < 16; b++){
                used = used || e[b];
            }
            if(!used){
                continue;
            }
            uint64_t first = le64(e + 32);
            uint64_t last = le64(e + 40);
            if(last < first || last >= dev->block_count()){
                kprintf("partition: intrarea GPT %u invalida, ignorata\n", i + 1);
                continue;
            }
            PartInfo& p = s.out[s.n++];
            memset(&p, 0, sizeof(p));
            p.number = (uint8_t)(i + 1);
            p.gpt = true;
            p.start = first;
            p.count = last - first + 1;
            memcpy(p.type_guid, e, 16);
            memcpy(p.part_guid, e + 16, 16);
        }
        result = s.n - before;
    } while(0);
    kfree(hdr);
    if(ents){
        kfree(ents);
    }
    return result;
}

int scan_gpt(Scan& s){
    int r = scan_gpt_at(s, 1);
    if(r < 0 && s.dev->block_count() > 2){
        r = scan_gpt_at(s, s.dev->block_count() - 1);
        if(r >= 0){
            kprintf("partition: antetul GPT principal e stricat, folosit cel de rezerva\n");
        }
    }
    return r;
}

// Lantul EBR al partitiei extinse: logicele primesc numerele 5, 6, ...
void scan_logical(Scan& s, uint8_t* sect, uint64_t ext_start, uint64_t ext_count){
    uint64_t ebr = ext_start;
    uint8_t number = 5;
    for(int i = 0; i < 32; i++){
        if(!read_sectors(s.dev, ebr, 1, sect) || sect[510] != 0x55 || sect[511] != 0xAA){
            return;
        }
        const uint8_t* e0 = sect + 446;
        const uint8_t* e1 = sect + 446 + 16;
        uint64_t lstart = ebr + le32(e0 + 8);
        if(e0[4] && !is_extended(e0[4]) && lstart < ext_start + ext_count){
            s.add_mbr(number, e0[4], lstart, le32(e0 + 12));
        }
        number++;
        if(!is_extended(e1[4]) || !le32(e1 + 8)){
            return;
        }
        uint64_t next = ext_start + le32(e1 + 8);
        if(next <= ebr){
            return;
        }
        ebr = next;
    }
}

}

uint32_t crc32(const void* data, size_t len){
    const uint8_t* p = (const uint8_t*)data;
    uint32_t c = 0xFFFFFFFF;
    for(size_t i = 0; i < len; i++){
        c ^= p[i];
        for(int b = 0; b < 8; b++){
            c = (c >> 1) ^ (0xEDB88320 & (0 - (c & 1)));
        }
    }
    return ~c;
}

int partition_scan(BlockDevice* dev, PartInfo* out, int max, PartScheme* scheme){
    if(scheme){
        *scheme = PartScheme::None;
    }
    uint32_t bs = dev->block_size();
    if(!valid_bs(bs) || dev->block_count() < 2){
        return 0;
    }
    uint8_t* sect = (uint8_t*)kmalloc(bs);
    if(!sect){
        return 0;
    }
    Scan s{dev, out, max, 0};
    if(read_sectors(dev, 0, 1, sect) && sect[510] == 0x55 && sect[511] == 0xAA){
        bool protective = false;
        for(int i = 0; i < 4; i++){
            protective = protective || sect[446 + 16 * i + 4] == 0xEE;
        }
        if(protective && scan_gpt(s) >= 0){
            if(scheme){
                *scheme = PartScheme::Gpt;
            }
        } else {
            uint8_t entries[64];
            memcpy(entries, sect + 446, 64);
            if(scheme){
                *scheme = PartScheme::Mbr;
            }
            for(int i = 0; i < 4; i++){
                const uint8_t* e = entries + 16 * i;
                uint8_t type = e[4];
                uint64_t start = le32(e + 8);
                uint64_t count = le32(e + 12);
                if(type == 0xEE){
                    continue;
                }
                if(is_extended(type)){
                    if(start && count && start + count <= dev->block_count()){
                        scan_logical(s, sect, start, count);
                    }
                    continue;
                }
                s.add_mbr((uint8_t)(i + 1), type, start, count);
            }
            // primarele au numerele 1-4, logicele 5+; extinsa poate sta oriunde in tabela
            for(int i = 1; i < s.n; i++){
                PartInfo t = s.out[i];
                int j = i - 1;
                while(j >= 0 && s.out[j].number > t.number){
                    s.out[j + 1] = s.out[j];
                    j--;
                }
                s.out[j + 1] = t;
            }
        }
    }
    kfree(sect);
    return s.n;
}

PartitionDevice::PartitionDevice(BlockDevice* parent_dev, const PartInfo& pi, const char* base_name)
    : parent(parent_dev), start(pi.start), count(pi.count), info(pi){
    int n = 0;
    while(base_name[n] && n < 10){
        name[n] = base_name[n];
        n++;
    }
    if(n && base_name[n - 1] >= '0' && base_name[n - 1] <= '9'){
        name[n++] = 'p';
    }
    uint8_t v = pi.number;
    char digits[4];
    int d = 0;
    do {
        digits[d++] = '0' + v % 10;
        v /= 10;
    } while(v);
    while(d){
        name[n++] = digits[--d];
    }
    name[n] = 0;
}

int PartitionDevice::read(uint64_t lba, uint32_t n, void* buf){
    if(lba >= count || n > count - lba){
        return n ? -EINVAL : 0;
    }
    return parent->read(start + lba, n, buf);
}

int PartitionDevice::write(uint64_t lba, uint32_t n, const void* buf){
    if(lba >= count || n > count - lba){
        return n ? -EINVAL : 0;
    }
    return parent->write(start + lba, n, buf);
}

int partition_register(BlockDevice* disk, const char* base_name){
    PartInfo parts[PART_MAX];
    PartScheme scheme;
    int n = partition_scan(disk, parts, PART_MAX, &scheme);
    int added = 0;
    for(int i = 0; i < n && registered_count < MAX_PARTS_REGISTERED; i++){
        PartitionDevice* p = new PartitionDevice(disk, parts[i], base_name);
        if(!hal_register(HalKind::Block, p, p->name)){
            kprintf("partition: registrul HAL e plin, %s ignorata\n", p->name);
            continue;
        }
        registered[registered_count++] = p;
        added++;
        kprintf("partition: %s %s start=%lu sectoare=%lu\n", p->name, scheme == PartScheme::Gpt ? "GPT" : "MBR",
                p->start, p->count);
    }
    return added;
}

const PartitionDevice* partition_find(const BlockDevice* dev){
    for(int i = 0; i < registered_count; i++){
        if(registered[i] == dev){
            return registered[i];
        }
    }
    return nullptr;
}

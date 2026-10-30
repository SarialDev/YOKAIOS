#include "tests/test_block.h"
#include "block/blockdev.h"
#include "block/bcache.h"
#include "block/partition.h"
#include "core/panic.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_block:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_block:%d: %s", __LINE__, #cond); } } while(0)

const uint32_t SECTORS = 512;
uint8_t disk_data[SECTORS * 512];

class FakeDisk final : public BlockDevice {
public:
    constexpr FakeDisk() = default;
    uint32_t block_size() const override { return 512; }
    uint64_t block_count() const override { return SECTORS; }
    int read(uint64_t lba, uint32_t count, void* buf) override {
        reads++;
        if(fail_reads || lba + count > SECTORS){
            return -EIO;
        }
        memcpy(buf, disk_data + lba * 512, (size_t)count * 512);
        return 0;
    }
    int write(uint64_t lba, uint32_t count, const void* buf) override {
        writes++;
        if(lba + count > SECTORS){
            return -EIO;
        }
        memcpy(disk_data + lba * 512, buf, (size_t)count * 512);
        return 0;
    }
    int flush() override { flushes++; return 0; }
    int reads = 0;
    int writes = 0;
    int flushes = 0;
    bool fail_reads = false;
};

FakeDisk fake;

bool strcmp_eq(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

void fill_pattern(){
    for(uint32_t i = 0; i < sizeof(disk_data); i++){
        disk_data[i] = (uint8_t)(i * 13 + 7);
    }
    fake.reads = fake.writes = fake.flushes = 0;
    fake.fail_reads = false;
}

const uint8_t* blockp(uint64_t blk){
    return disk_data + blk * BCACHE_BLOCK;
}

// --- buffer cache ---

void check_cache_basic(){
    fill_pattern();
    BufferCache bc(4);
    Buf* b = bc.read(&fake, 3);
    CHECK(b);
    CHECK(memcmp(b->data, blockp(3), BCACHE_BLOCK) == 0);
    EQ(fake.reads, 1);
    bc.release(b);
    b = bc.read(&fake, 3);
    CHECK(b);
    EQ(fake.reads, 1);                      // din cache
    BcacheStats s = bc.stats();
    EQ(s.hits, 1);
    EQ(s.misses, 1);
    EQ(s.pinned, 1);
    bc.release(b);
    CHECK(bc.read(&fake, SECTORS / 8) == nullptr);      // dincolo de dispozitiv
    fake.fail_reads = true;
    CHECK(bc.read(&fake, 5) == nullptr);                // eroare de I/O
    fake.fail_reads = false;
    b = bc.read(&fake, 5);
    CHECK(b && memcmp(b->data, blockp(5), BCACHE_BLOCK) == 0);
    bc.release(b);
    b = bc.get(&fake, 6);                               // fara citire
    CHECK(b);
    int reads = fake.reads;
    bc.release(b);
    EQ(fake.reads, reads);
}

void check_cache_writeback(){
    fill_pattern();
    BufferCache bc(4);
    Buf* b = bc.read(&fake, 3);
    uint8_t old = b->data[0];
    b->data[0] = (uint8_t)(old ^ 0xFF);
    bc.mark_dirty(b);
    bc.release(b);
    EQ(fake.writes, 0);                                 // write-back: dispozitivul nu s-a schimbat
    CHECK(blockp(3)[0] == old);
    EQ(bc.stats().dirty, 1);
    EQ(bc.sync(&fake), 0);
    EQ(fake.writes, 1);
    EQ(fake.flushes, 1);
    CHECK(blockp(3)[0] == (uint8_t)(old ^ 0xFF));
    EQ(bc.stats().dirty, 0);
    EQ(bc.sync(&fake), 0);                              // nimic de scris, dar flush tot se da
    EQ(fake.writes, 1);
    EQ(fake.flushes, 2);

    // Scriere dintr-un bloc nou, necitit
    b = bc.get(&fake, 7);
    memset(b->data, 0x5A, BCACHE_BLOCK);
    bc.mark_dirty(b);
    bc.release(b);
    EQ(bc.sync(nullptr), 0);
    CHECK(blockp(7)[0] == 0x5A && blockp(7)[BCACHE_BLOCK - 1] == 0x5A && blockp(8)[0] != 0x5A);
    EQ(bc.stats().writebacks, 2);

    // Invalidare: blocul dispare din cache, continutul ramane pe disc
    b = bc.read(&fake, 3);
    Buf* pinned = b;
    EQ(bc.invalidate(&fake), -EBUSY);
    bc.release(pinned);
    EQ(bc.invalidate(&fake), 0);
    EQ(bc.stats().buffers, 0);
    b = bc.read(&fake, 3);
    CHECK(b->data[0] == (uint8_t)(old ^ 0xFF));
    bc.release(b);
}

void check_cache_lru(){
    fill_pattern();
    BufferCache bc(4);
    for(uint64_t i = 0; i < 4; i++){
        bc.release(bc.read(&fake, i));
    }
    bc.release(bc.read(&fake, 0));                      // 0 devine cel mai recent; cel mai vechi e 1
    int reads = fake.reads;
    bc.release(bc.read(&fake, 4));                      // scoate blocul 1
    EQ(bc.stats().evictions, 1);
    EQ(fake.reads, reads + 1);
    bc.release(bc.read(&fake, 0));
    bc.release(bc.read(&fake, 2));
    bc.release(bc.read(&fake, 3));
    EQ(fake.reads, reads + 1);                          // 0, 2, 3 au ramas
    bc.release(bc.read(&fake, 1));
    EQ(fake.reads, reads + 2);                          // 1 se citeste din nou

    // Un bloc murdar scos din cache se scrie intai
    fill_pattern();
    BufferCache bd(2);
    Buf* b = bd.read(&fake, 0);
    b->data[10] = 0x99;
    bd.mark_dirty(b);
    bd.release(b);
    bd.release(bd.read(&fake, 1));
    EQ(fake.writes, 0);
    bd.release(bd.read(&fake, 2));                      // scoate blocul 0
    EQ(fake.writes, 1);
    CHECK(blockp(0)[10] == 0x99);
    EQ(bd.stats().writebacks, 1);

    // Cache plin de blocuri pinned
    Buf* a = bd.read(&fake, 3);
    Buf* c = bd.read(&fake, 4);
    CHECK(a && c);
    CHECK(bd.read(&fake, 5) == nullptr);
    bd.release(a);
    b = bd.read(&fake, 5);
    CHECK(b);
    bd.release(b);
    bd.release(c);
}

void check_cache_aged(){
    fill_pattern();
    BufferCache bc(4);
    Buf* b = bc.read(&fake, 2);
    b->data[1] ^= 0xFF;
    bc.mark_dirty(b);
    bc.release(b);
    EQ(bc.writeback_aged(60000), 0);                    // prea recent
    EQ(fake.writes, 0);
    EQ(bc.writeback_aged(0), 1);
    EQ(fake.writes, 1);
    EQ(bc.writeback_aged(0), 0);
}

// --- partitii ---

void put32(uint8_t* p, uint32_t v){
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

void put64(uint8_t* p, uint64_t v){
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

uint8_t* sector(uint64_t lba){
    return disk_data + lba * 512;
}

void mbr_entry(uint8_t* sect, int slot, uint8_t type, uint32_t start, uint32_t count){
    uint8_t* e = sect + 446 + 16 * slot;
    e[4] = type;
    put32(e + 8, start);
    put32(e + 12, count);
    sect[510] = 0x55;
    sect[511] = 0xAA;
}

void build_mbr(){
    memset(disk_data, 0, sizeof(disk_data));
    mbr_entry(sector(0), 0, 0x83, 8, 56);
    mbr_entry(sector(0), 1, 0x0C, 64, 64);
    mbr_entry(sector(0), 2, 0x05, 128, 384);            // extinsa: 128..511
    mbr_entry(sector(128), 0, 0x83, 2, 30);             // logica 5 la 130
    mbr_entry(sector(128), 1, 0x05, 64, 64);            // urmatorul EBR la 128 + 64
    mbr_entry(sector(192), 0, 0x83, 2, 30);             // logica 6 la 194
}

void check_mbr(){
    build_mbr();
    PartInfo p[PART_MAX];
    PartScheme scheme;
    int n = partition_scan(&fake, p, PART_MAX, &scheme);
    EQ(n, 4);
    CHECK(scheme == PartScheme::Mbr);
    EQ(p[0].number, 1); EQ(p[0].start, 8); EQ(p[0].count, 56); EQ(p[0].type, 0x83);
    EQ(p[1].number, 2); EQ(p[1].start, 64); EQ(p[1].count, 64); EQ(p[1].type, 0x0C);
    EQ(p[2].number, 5); EQ(p[2].start, 130); EQ(p[2].count, 30);
    EQ(p[3].number, 6); EQ(p[3].start, 194); EQ(p[3].count, 30);

    // Partitie care iese din disc: ignorata; fara semnatura 55AA: nicio tabela
    mbr_entry(sector(0), 3, 0x83, 500, 100);
    EQ(partition_scan(&fake, p, PART_MAX, &scheme), 4);
    sector(0)[511] = 0;
    EQ(partition_scan(&fake, p, PART_MAX, &scheme), 0);
    CHECK(scheme == PartScheme::None);
    EQ(partition_scan(&fake, p, 2, &scheme), 0);
}

const uint8_t LINUX_GUID[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};

void gpt_header(uint64_t my, uint64_t alt, uint64_t table_lba, uint32_t table_crc){
    uint8_t* h = sector(my);
    memset(h, 0, 512);
    memcpy(h, "EFI PART", 8);
    put32(h + 8, 0x00010000);
    put32(h + 12, 92);
    put64(h + 24, my);
    put64(h + 32, alt);
    put64(h + 40, 34);
    put64(h + 48, SECTORS - 34);
    put64(h + 72, table_lba);
    put32(h + 80, 128);
    put32(h + 84, 128);
    put32(h + 88, table_crc);
    put32(h + 16, crc32(h, 92));
}

void build_gpt(){
    memset(disk_data, 0, sizeof(disk_data));
    uint8_t* mbr = sector(0);
    mbr_entry(mbr, 0, 0xEE, 1, SECTORS - 1);
    uint8_t* ents = sector(2);
    for(int i = 0; i < 3; i += 2){
        uint8_t* e = ents + 128 * i;
        memcpy(e, LINUX_GUID, 16);
        e[16] = (uint8_t)(0xA0 + i);
        put64(e + 32, i == 0 ? 40 : 100);
        put64(e + 40, i == 0 ? 99 : 199);
    }
    uint32_t tcrc = crc32(ents, 128 * 128);
    memcpy(sector(SECTORS - 33), ents, 128 * 128);
    gpt_header(1, SECTORS - 1, 2, tcrc);
    gpt_header(SECTORS - 1, 1, SECTORS - 33, tcrc);
}

void check_gpt(){
    build_gpt();
    PartInfo p[PART_MAX];
    PartScheme scheme;
    int n = partition_scan(&fake, p, PART_MAX, &scheme);
    EQ(n, 2);
    CHECK(scheme == PartScheme::Gpt);
    CHECK(p[0].gpt && p[1].gpt);
    EQ(p[0].number, 1); EQ(p[0].start, 40); EQ(p[0].count, 60);
    EQ(p[1].number, 3); EQ(p[1].start, 100); EQ(p[1].count, 100);
    CHECK(memcmp(p[0].type_guid, LINUX_GUID, 16) == 0);
    EQ(p[0].part_guid[0], 0xA0);
    EQ(p[1].part_guid[0], 0xA2);

    // Antet principal cu CRC gresit: se foloseste cel de rezerva
    sector(1)[20] ^= 1;
    EQ(partition_scan(&fake, p, PART_MAX, &scheme), 2);
    CHECK(scheme == PartScheme::Gpt);
    // Tabela principala stricata, antetul bun: CRC-ul tabelei nu se potriveste -> tot rezerva
    sector(1)[20] ^= 1;
    sector(2)[16] ^= 0xFF;
    EQ(partition_scan(&fake, p, PART_MAX, &scheme), 2);
    EQ(p[0].part_guid[0], 0xA0);
    // Ambele stricate: tabela GPT nu exista; intrarea protectiva 0xEE nu da partitii
    sector(SECTORS - 1)[20] ^= 1;
    EQ(partition_scan(&fake, p, PART_MAX, &scheme), 0);
    CHECK(scheme == PartScheme::Mbr);
}

void check_partition_device(){
    build_mbr();
    fill_pattern();
    PartInfo info;
    memset(&info, 0, sizeof(info));
    info.number = 2;
    info.start = 64;
    info.count = 64;
    PartitionDevice p(&fake, info, "sda");
    CHECK(strcmp_eq(p.name, "sda2"));
    PartitionDevice q(&fake, info, "nvme0n1");
    CHECK(strcmp_eq(q.name, "nvme0n1p2"));
    EQ(p.block_count(), 64);
    EQ(p.block_size(), 512);
    uint8_t buf[1024];
    EQ(p.read(0, 2, buf), 0);
    CHECK(memcmp(buf, disk_data + 64 * 512, 1024) == 0);
    memset(buf, 0xC3, sizeof(buf));
    EQ(p.write(62, 2, buf), 0);
    CHECK(sector(126)[0] == 0xC3 && sector(127)[511] == 0xC3 && sector(128)[0] != 0xC3);
    EQ(p.write(63, 2, buf), -EINVAL);                   // peste capatul partitiei
    EQ(p.read(64, 1, buf), -EINVAL);
    EQ(p.flush(), 0);
    EQ(fake.flushes, 1);
}

}

void test_block_run(){
    check_cache_basic();
    kprintf("[OK] test_block cache\n");
    check_cache_writeback();
    kprintf("[OK] test_block write-back\n");
    check_cache_lru();
    kprintf("[OK] test_block lru\n");
    check_cache_aged();
    kprintf("[OK] test_block aged\n");
    check_mbr();
    kprintf("[OK] test_block mbr\n");
    check_gpt();
    kprintf("[OK] test_block gpt\n");
    check_partition_device();
    kprintf("[OK] test_block partition\n");
    kprintf("[OK] test_block (cache LRU 4 KiB write-back, MBR cu logice, GPT cu CRC si antet de rezerva)\n");
}

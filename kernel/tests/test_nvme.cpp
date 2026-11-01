#include "tests/test_nvme.h"
#include "drivers/nvme/nvme.h"
#include "block/blockdev.h"
#include "block/bcache.h"
#include "block/partition.h"
#include "hal/hal.h"
#include "mm/kmalloc.h"
#include "core/panic.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "time/clock.h"
#include "lib/string.h"
#include "lib/kprintf.h"
#include "uapi/syscall_nr.h"

namespace {

#define EQ(expr, want) do { \
        int64_t got_ = (int64_t)(expr); int64_t want_ = (int64_t)(want); \
        if(got_ != want_){ panic("test_nvme:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_nvme:%d: %s", __LINE__, #cond); } } while(0)

// nvme0n1 = imaginea tests/mkdisk.py gpt (32 MiB x 512 B); nvme0n2 = tests/mknvme.py (4 MiB x 4096 B)
const uint64_t DISK_SECTORS = 65536;
const uint64_t NS2_BLOCKS = 1024;

bool name_eq(const char* a, const char* b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

BlockDevice* find_block(const char* name){
    for(int i = 0; i < hal_count(HalKind::Block); i++){
        if(name_eq(hal_name(HalKind::Block, i), name)){
            return hal_block(i);
        }
    }
    return nullptr;
}

void fill(uint8_t* p, size_t n, uint8_t seed){
    for(size_t i = 0; i < n; i++){
        p[i] = (uint8_t)(seed + i * 31 + (i >> 9));
    }
}

void check_identify(BlockDevice* n1, BlockDevice* n2){
    EQ(nvme_controller_count(), 1);
    EQ(nvme_ns_count(), 2);
    CHECK(n1 == nvme_ns(0) && n2 == nvme_ns(1));
    EQ(n1->block_size(), 512);
    EQ(n1->block_count(), DISK_SECTORS);
    EQ(n2->block_size(), 4096);
    EQ(n2->block_count(), NS2_BLOCKS);
    EQ(nvme_irq_mode(0), 2);                                   // QEMU nvme are MSI-X
    uint8_t* buf = (uint8_t*)kmalloc(4096);
    CHECK(buf);
    EQ(n1->read(0, 1, buf), 0);
    CHECK(memcmp(buf, "YOKAI-AHCI-SECTOR0", 18) == 0);
    for(int i = 18; i < 446; i++){
        if(buf[i] != (uint8_t)(i * 7 + 3)){
            panic("test_nvme: sector 0 octet %d = %x", i, buf[i]);
        }
    }
    CHECK(buf[510] == 0x55 && buf[511] == 0xAA);
    EQ(n2->read(0, 1, buf), 0);
    CHECK(memcmp(buf, "YOKAI-NVME-NS2", 14) == 0);
    for(int i = 14; i < 4096; i++){
        if(buf[i] != (uint8_t)(i * 3 + 1)){
            panic("test_nvme: ns2 octet %d = %x", i, buf[i]);
        }
    }
    kfree(buf);
    kprintf("[OK] test_nvme identify (nvme0n1 512 B, nvme0n2 4096 B)\n");
}

void check_partitions(BlockDevice* n1){
    struct Expected { const char* name; uint64_t start; uint64_t count; };
    static const Expected parts[] = {{"nvme0n1p1", 2048, 16384}, {"nvme0n1p2", 18432, 32768}};
    for(int i = 0; i < 2; i++){
        BlockDevice* d = find_block(parts[i].name);
        if(!d){
            panic("test_nvme: lipseste %s", parts[i].name);
        }
        const PartitionDevice* pd = partition_find(d);
        CHECK(pd && pd->parent == n1);
        EQ(pd->start, parts[i].start);
        EQ(pd->count, parts[i].count);
        uint8_t buf[512];
        EQ(d->read(0, 1, buf), 0);
        CHECK(memcmp(buf, "YKPART", 6) == 0);
        EQ(buf[6], '1' + i);
    }
    CHECK(find_block("nvme0n1p3") == nullptr && find_block("nvme0n2p1") == nullptr);
    kprintf("[OK] test_nvme partitii GPT 2\n");
}

// Scrie si citeste inapoi dimensiuni si aliniamente care dau 1 pagina, 2 pagini, lista PRP, cereri multiple
void check_rw(BlockDevice* dev, uint64_t base_lba){
    uint32_t bs = dev->block_size();
    const uint32_t MAXB = 160 * 1024;
    uint8_t* wraw = (uint8_t*)kmalloc(MAXB + 2 * 4096);
    uint8_t* rraw = (uint8_t*)kmalloc(MAXB + 2 * 4096);
    CHECK(wraw && rraw);
    uint8_t* wa = (uint8_t*)(((uint64_t)wraw + 4095) & ~4095ull);
    uint8_t* ra = (uint8_t*)(((uint64_t)rraw + 4095) & ~4095ull);
    static const uint32_t counts512[] = {1, 7, 8, 9, 16, 17, 255, 256, 257, 300};
    static const uint32_t counts4k[] = {1, 2, 3, 5, 8, 31, 32, 33, 40};
    const uint32_t* counts = bs == 512 ? counts512 : counts4k;
    int nc = bs == 512 ? 10 : 9;
    static const uint32_t offsets[] = {0, 4, 508, 2048, 4092};
    uint64_t lba = base_lba;
    int cases = 0;
    for(int ci = 0; ci < nc; ci++){
        uint32_t cnt = counts[ci];
        if((uint64_t)cnt * bs > MAXB){
            continue;
        }
        for(int oi = 0; oi < 5; oi++){
            uint8_t* w = wa + offsets[oi];
            uint8_t* r = ra + (4092 - offsets[oi]);
            fill(w, (uint64_t)cnt * bs, (uint8_t)(cases * 13 + 1));
            memset(r, 0, (uint64_t)cnt * bs);
            EQ(dev->write(lba, cnt, w), 0);
            EQ(dev->read(lba, cnt, r), 0);
            if(memcmp(w, r, (uint64_t)cnt * bs) != 0){
                panic("test_nvme: citire != scriere (cnt=%u offset=%u)", cnt, offsets[oi]);
            }
            lba += cnt + 1;
            cases++;
        }
    }
    // Adresa nealiniata la dword si cereri invalide
    EQ(dev->read(lba, 1, ra + 1), -EINVAL);
    EQ(dev->read(dev->block_count(), 1, ra), -EINVAL);
    EQ(dev->read(dev->block_count() - 1, 2, ra), -EINVAL);
    EQ(dev->write(lba, 0, wa), 0);
    EQ(dev->flush(), 0);
    kfree(wraw);
    kfree(rraw);
}

void check_cache(BlockDevice* dev){
    uint32_t per = BCACHE_BLOCK / dev->block_size();
    Buf* b = bread(dev, 20);
    CHECK(b);
    uint8_t* raw = (uint8_t*)kmalloc(4096);
    uint8_t* want = (uint8_t*)kmalloc(4096);
    CHECK(raw && want);
    EQ(dev->read(20 * per, per, raw), 0);
    CHECK(memcmp(raw, b->data, 4096) == 0);
    fill(b->data, 4096, 0x3C);
    bdirty(b);
    brelse(b);
    fill(want, 4096, 0x3C);
    EQ(dev->read(20 * per, per, raw), 0);
    CHECK(memcmp(raw, want, 4096) != 0);                       // inca doar in cache
    EQ(bsync(dev), 0);
    EQ(dev->read(20 * per, per, raw), 0);
    CHECK(memcmp(raw, want, 4096) == 0);                       // scris pe disc
    EQ(bcache().invalidate(dev), 0);
    b = bread(dev, 20);
    CHECK(b && memcmp(b->data, want, 4096) == 0);
    brelse(b);
    EQ(bcache().invalidate(dev), 0);
    kfree(raw);
    kfree(want);
}

struct Job {
    BlockDevice* n1;
    BlockDevice* n2;
    volatile bool done;
};

void nvme_task(void* arg){
    Job* j = (Job*)arg;
    check_rw(j->n1, 40000);
    check_rw(j->n2, 100);
    check_cache(find_block("nvme0n1p2"));
    j->done = true;
}

}

void test_nvme_run(){
    if(nvme_controller_count() < 1){
        panic("test_nvme: niciun controller NVMe (QEMU: -device nvme + -device nvme-ns)");
    }
    BlockDevice* n1 = find_block("nvme0n1");
    BlockDevice* n2 = find_block("nvme0n2");
    CHECK(n1 && n2);
    check_identify(n1, n2);
    check_partitions(n1);

    // Task-ul de boot (idle) nu se blocheaza, deci prima trecere foloseste interogarea; a doua ruleaza intr-un
    // task obisnuit, care asteapta terminarea prin MSI-X
    uint32_t irqs0 = nvme_irq_count();
    check_rw(n1, 1000);
    check_rw(n2, 10);
    EQ(nvme_irq_count(), irqs0);
    kprintf("[OK] test_nvme prp poll\n");

    // Scrierile prin partitie si prin namespace-ul intreg vad aceleasi date; ns2 e separat de ns1
    BlockDevice* p2 = find_block("nvme0n1p2");
    CHECK(p2);
    const PartitionDevice* pd = partition_find(p2);
    uint8_t a[512], b[512];
    fill(a, 512, 0x55);
    EQ(p2->write(7, 1, a), 0);
    EQ(n1->read(pd->start + 7, 1, b), 0);
    CHECK(memcmp(a, b, 512) == 0);
    uint8_t* s0 = (uint8_t*)kmalloc(4096);
    CHECK(s0);
    EQ(n2->read(0, 1, s0), 0);
    CHECK(memcmp(s0, "YOKAI-NVME-NS2", 14) == 0);
    kfree(s0);

    Job job = {n1, n2, false};
    Task* t = task_create_kernel("t-nvme", nvme_task, &job);
    CHECK(t);
    sched_add(t);
    // La boot intreruperile sunt oprite, iar hlt-ul din sleep_ms le cere pornite
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    for(int i = 0; i < 800 && !job.done; i++){
        sleep_ms(25);
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    CHECK(job.done);
    CHECK(nvme_irq_count() > irqs0);
    kprintf("[OK] test_nvme prp msi-x (%u intreruperi)\n", nvme_irq_count() - irqs0);
    kprintf("[OK] test_nvme cache\n");
    kprintf("[OK] test_nvme (NVMe: IDENTIFY, 2 namespace-uri, PRP 1/2/lista, READ/WRITE/FLUSH, MSI-X, partitii, cache)\n");
}

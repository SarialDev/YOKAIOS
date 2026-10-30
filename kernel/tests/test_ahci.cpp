#include "tests/test_ahci.h"
#include "drivers/ahci/ahci.h"
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
        if(got_ != want_){ panic("test_ahci:%d: %s = %ld, asteptat %ld", __LINE__, #expr, got_, want_); } \
    } while(0)
#define CHECK(cond) do { if(!(cond)){ panic("test_ahci:%d: %s", __LINE__, #cond); } } while(0)

// Imaginea vine din tests/mkdisk.py: 32 MiB, sectorul 0 = "YOKAI-AHCI-SECTOR0" + (i*7+3), tabela, 55AA
const uint64_t DISK_SECTORS = 65536;

struct Expected {
    const char* name;
    uint64_t start;
    uint64_t count;
};

const Expected GPT_PARTS[] = {{"sda1", 2048, 16384}, {"sda2", 18432, 32768}};
const Expected MBR_PARTS[] = {{"sda1", 2048, 16384}, {"sda2", 18432, 16384}, {"sda5", 34818, 10238}, {"sda6", 45058, 10238}};

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

void check_sector0(BlockDevice* sda){
    EQ(sda->block_size(), 512);
    EQ(sda->block_count(), DISK_SECTORS);
    uint8_t* buf = (uint8_t*)kmalloc(512);
    CHECK(buf);
    EQ(sda->read(0, 1, buf), 0);
    CHECK(memcmp(buf, "YOKAI-AHCI-SECTOR0", 18) == 0);
    for(int i = 18; i < 446; i++){
        if(buf[i] != (uint8_t)(i * 7 + 3)){
            panic("test_ahci: sector 0 octet %d = %x", i, buf[i]);
        }
    }
    CHECK(buf[510] == 0x55 && buf[511] == 0xAA);
    kfree(buf);
}

void check_partitions(BlockDevice* sda){
    BlockDevice* p1 = find_block("sda1");
    CHECK(p1);
    const PartitionDevice* pd = partition_find(p1);
    CHECK(pd);
    bool gpt = pd->info.gpt;
    const Expected* want = gpt ? GPT_PARTS : MBR_PARTS;
    int n = gpt ? 2 : 4;
    for(int i = 0; i < n; i++){
        BlockDevice* d = find_block(want[i].name);
        if(!d){
            panic("test_ahci: lipseste %s", want[i].name);
        }
        pd = partition_find(d);
        CHECK(pd && pd->parent == sda);
        EQ(pd->start, want[i].start);
        EQ(pd->count, want[i].count);
        // Prima sectoare a partitiei: "YKPART" + cifra din nume
        uint8_t buf[512];
        EQ(d->read(0, 1, buf), 0);
        CHECK(memcmp(buf, "YKPART", 6) == 0);
        EQ(buf[6], want[i].name[3]);
        uint8_t raw[512];
        EQ(sda->read(want[i].start, 1, raw), 0);
        CHECK(memcmp(buf, raw, 512) == 0);
    }
    CHECK(find_block("sda3") == nullptr && find_block("sda7") == nullptr);
    kprintf("[OK] test_ahci partitii %s %d\n", gpt ? "GPT" : "MBR", n);
}

struct Job {
    BlockDevice* sda;
    BlockDevice* last;
    volatile bool done;
};

void fill(uint8_t* p, size_t n, uint8_t seed){
    for(size_t i = 0; i < n; i++){
        p[i] = (uint8_t)(seed + i * 31 + (i >> 9));
    }
}

void check_rw(BlockDevice* sda, BlockDevice* last){
    const uint32_t N = 300;                             // 150 KiB: doua cereri DMA (128 KiB + rest)
    uint8_t* w = (uint8_t*)kmalloc(N * 512 + 2);
    uint8_t* r = (uint8_t*)kmalloc(N * 512 + 2);
    CHECK(w && r);
    fill(w, N * 512 + 2, 0x11);
    // Scriere/citire aliniata, apoi la adresa pare nealiniata la pagina (segmente cu margini ciudate)
    EQ(last->write(1000, N, w), 0);
    memset(r, 0, N * 512);
    EQ(last->read(1000, N, r), 0);
    CHECK(memcmp(w, r, N * 512) == 0);
    fill(w + 2, N * 512, 0x77);
    EQ(last->write(5000, N, w + 2), 0);
    EQ(last->read(5000, N, r + 2), 0);
    CHECK(memcmp(w + 2, r + 2, N * 512) == 0);
    // Aceleasi date, vazute prin discul intreg: partitia incepe la start
    const PartitionDevice* pd = partition_find(last);
    EQ(sda->read(pd->start + 1000, 1, r), 0);
    fill(w, 512, 0x11);
    CHECK(memcmp(w, r, 512) == 0);
    // Adresa impara si cereri invalide
    EQ(last->read(0, 1, r + 1), -EINVAL);
    EQ(last->read(last->block_count(), 1, r), -EINVAL);
    EQ(last->read(last->block_count() - 1, 2, r), -EINVAL);
    EQ(sda->read(DISK_SECTORS - 1, 1, r), 0);
    EQ(last->write(1, 0, w), 0);
    EQ(last->flush(), 0);
    kfree(w);
    kfree(r);
}

void check_cache(BlockDevice* last){
    Buf* b = bread(last, 20);
    CHECK(b);
    uint8_t raw[4096];
    EQ(last->read(20 * 8, 8, raw), 0);
    CHECK(memcmp(raw, b->data, 4096) == 0);
    fill(b->data, 4096, 0x3C);
    bdirty(b);
    brelse(b);
    EQ(last->read(20 * 8, 8, raw), 0);
    uint8_t want[4096];
    fill(want, 4096, 0x3C);
    CHECK(memcmp(raw, want, 4096) != 0);                // inca doar in cache
    EQ(bsync(last), 0);
    EQ(last->read(20 * 8, 8, raw), 0);
    CHECK(memcmp(raw, want, 4096) == 0);                // scris pe disc
    EQ(bcache().invalidate(last), 0);
    b = bread(last, 20);
    CHECK(b && memcmp(b->data, want, 4096) == 0);
    brelse(b);
    EQ(bcache().invalidate(last), 0);
}

void ahci_task(void* arg){
    Job* j = (Job*)arg;
    check_rw(j->sda, j->last);
    check_cache(j->last);
    j->done = true;
}

}

void test_ahci_run(){
    if(ahci_disk_count() < 1){
        panic("test_ahci: niciun disc AHCI (QEMU: -device ahci + -device ide-hd)");
    }
    BlockDevice* sda = find_block("sda");
    CHECK(sda && sda == ahci_disk(0));
    check_sector0(sda);
    kprintf("[OK] test_ahci sector0\n");
    check_partitions(sda);

    BlockDevice* last = find_block("sda2");
    CHECK(last);
    // Task-ul de boot (idle) nu se blocheaza, deci prima trecere foloseste interogarea; a doua ruleaza intr-un
    // task obisnuit, care asteapta terminarea prin MSI
    uint32_t irqs0 = ahci_irq_count();
    check_rw(sda, last);
    EQ(ahci_irq_count(), irqs0);
    kprintf("[OK] test_ahci dma poll\n");
    Job job = {sda, last, false};
    Task* t = task_create_kernel("t-ahci", ahci_task, &job);
    CHECK(t);
    sched_add(t);
    // La boot intreruperile sunt oprite, iar hlt-ul din sleep_ms le cere pornite
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; sti" : "=r"(flags) : : "memory");
    for(int i = 0; i < 400 && !job.done; i++){
        sleep_ms(25);
    }
    if(!(flags & (1UL << 9))){
        __asm__ volatile("cli" : : : "memory");
    }
    CHECK(job.done);
    CHECK(ahci_irq_count() > irqs0);
    kprintf("[OK] test_ahci dma irq (%u intreruperi)\n", ahci_irq_count() - irqs0);
    kprintf("[OK] test_ahci cache\n");
    kprintf("[OK] test_ahci (AHCI: IDENTIFY, READ/WRITE DMA EXT multi-segment, FLUSH, MSI, partitii, cache)\n");
}

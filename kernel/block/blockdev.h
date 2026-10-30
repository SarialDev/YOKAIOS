#pragma once
#include "types.h"
#include "hal/block.h"
#include "sync/spinlock.h"
#include "sync/semaphore.h"

// Coada de cereri pentru un dispozitiv bloc: cererile se executa pe rand (fara NCQ si fara planificare),
// task-ul care a cerut asteapta terminarea (IRQ) sau interogheaza hardware-ul cand intreruperile sunt oprite

enum class BlockOp : uint8_t { Read, Write, Flush };

struct BlockRequest {
    BlockRequest* next;
    uint64_t lba;
    uint32_t count;
    void* buf;
    BlockOp op;
    int status;                 // 0 sau -errno, valabil dupa done
    Completion done;
    BlockRequest(BlockOp o, uint64_t l, uint32_t c, void* b)
        : next(nullptr), lba(l), count(c), buf(b), op(o), status(0), done(){}
};

struct BlockQueueOps {
    // Cu lock-ul cozii tinut si intreruperile oprite; 0 = cererea a pornit, negativ = respinsa pe loc
    int (*start)(void* ctx, BlockRequest* r);
    // Verifica hardware-ul; la terminare apeleaza finish_locked (lock tinut)
    void (*poll)(void* ctx);
    // Timeout: readuce hardware-ul intr-o stare utilizabila (lock tinut)
    void (*abort)(void* ctx);
};

class BlockQueue {
public:
    BlockQueue() : lock("blockq"), ops(), ctx(nullptr), use_irq(false), head(nullptr), tail(nullptr), cur(nullptr){}
    void init(const BlockQueueOps& o, void* c, bool irq){ ops = o; ctx = c; use_irq = irq; }
    // Pune cererea in coada si asteapta; intoarce r->status (-EIO la timeout)
    int submit_wait(BlockRequest* r, uint64_t timeout_ms = 5000);
    // Doar cu lock-ul cozii tinut (handler de IRQ, poll, abort)
    void finish_locked(int status);
    BlockRequest* current_locked() const { return cur; }
    Spinlock lock;
private:
    void start_next_locked();
    BlockQueueOps ops;
    void* ctx;
    bool use_irq;
    BlockRequest* head;
    BlockRequest* tail;
    BlockRequest* cur;
};

// Citeste/scrie octeti arbitrari peste un dispozitiv cu sectoare (citeste-modifica-scrie la margini)
int block_read_bytes(BlockDevice* dev, uint64_t pos, void* buf, uint64_t len);
int block_write_bytes(BlockDevice* dev, uint64_t pos, const void* buf, uint64_t len);

// Linia idx din lsblk (toate dispozitivele bloc din HAL, discuri si partitii); false dupa ultima
bool block_format(int idx, char* buf, size_t n);
void block_dump();

#include "ipc/msgq.h"
#include "uapi/syscall_nr.h"
#include "sync/spinlock.h"
#include "sync/waitqueue.h"
#include "mm/kmalloc.h"
#include "lib/new.h"
#include "lib/string.h"

namespace {

const uint32_t MAX_MSG_SIZE = 4096;
const uint32_t MAX_DEPTH = 4096;

}

struct MsgQueue {
    Spinlock lock{"msgq"};
    WaitQueue not_empty;
    WaitQueue not_full;
    uint32_t size;
    uint32_t depth;
    uint32_t head = 0;
    uint32_t count = 0;
    uint32_t* lens;                 // lungimea fiecarui slot
    uint8_t* slots;                 // depth * size bytes

    uint8_t* slot(uint32_t i){
        return slots + (uint64_t)i * size;
    }
};

MsgQueue* msgq_create(uint32_t size, uint32_t depth){
    if(!size || size > MAX_MSG_SIZE || !depth || depth > MAX_DEPTH){
        return nullptr;
    }
    void* mem = kmalloc(sizeof(MsgQueue));
    uint32_t* lens = (uint32_t*)kmalloc(depth * sizeof(uint32_t));
    uint8_t* slots = (uint8_t*)kmalloc((uint64_t)size * depth);
    if(!mem || !lens || !slots){
        kfree(mem);
        kfree(lens);
        kfree(slots);
        return nullptr;
    }
    MsgQueue* q = new (mem) MsgQueue();
    q->size = size;
    q->depth = depth;
    q->lens = lens;
    q->slots = slots;
    return q;
}

void msgq_destroy(MsgQueue* q){
    kfree(q->slots);
    kfree(q->lens);
    q->~MsgQueue();
    kfree(q);
}

int msgq_send(MsgQueue* q, const void* msg, uint32_t len, bool block){
    if(len > q->size){
        return -EINVAL;
    }
    uint64_t flags = q->lock.lock_irqsave();
    while(q->count == q->depth){
        if(!block){
            q->lock.unlock_irqrestore(flags);
            return -EAGAIN;
        }
        q->not_full.wait(q->lock);
    }
    uint32_t i = (q->head + q->count) % q->depth;
    memcpy(q->slot(i), msg, len);
    q->lens[i] = len;
    q->count++;
    q->not_empty.wake_one();
    q->lock.unlock_irqrestore(flags);
    return 0;
}

int msgq_recv(MsgQueue* q, void* buf, uint32_t cap, bool block){
    uint64_t flags = q->lock.lock_irqsave();
    while(q->count == 0){
        if(!block){
            q->lock.unlock_irqrestore(flags);
            return -EAGAIN;
        }
        q->not_empty.wait(q->lock);
    }
    uint32_t len = q->lens[q->head];
    if(len > cap){
        q->lock.unlock_irqrestore(flags);
        return -EINVAL;
    }
    memcpy(buf, q->slot(q->head), len);
    q->head = (q->head + 1) % q->depth;
    q->count--;
    q->not_full.wake_one();
    q->lock.unlock_irqrestore(flags);
    return (int)len;
}

uint32_t msgq_count(const MsgQueue* q){
    return __atomic_load_n(&q->count, __ATOMIC_RELAXED);
}

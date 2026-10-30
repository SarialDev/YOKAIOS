#include "block/blockdev.h"
#include "block/partition.h"
#include "sched/task.h"
#include "time/clock.h"
#include "mm/kmalloc.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "uapi/syscall_nr.h"

namespace {

inline bool irqs_on(){
    uint64_t f;
    __asm__ volatile("pushfq; pop %0" : "=r"(f));
    return f & (1 << 9);
}

}

// Lock-ul cozii tinut
void BlockQueue::start_next_locked(){
    while(!cur && head){
        BlockRequest* r = head;
        head = r->next;
        if(!head){
            tail = nullptr;
        }
        r->next = nullptr;
        cur = r;
        int err = ops.start(ctx, r);
        if(err){
            cur = nullptr;
            r->status = err;
            r->done.complete();
        }
    }
}

void BlockQueue::finish_locked(int status){
    BlockRequest* r = cur;
    if(!r){
        return;
    }
    cur = nullptr;
    r->status = status;
    start_next_locked();
    r->done.complete();
}

int BlockQueue::submit_wait(BlockRequest* r, uint64_t timeout_ms){
    {
        SpinlockGuard g(lock);
        r->next = nullptr;
        if(tail){
            tail->next = r;
        } else {
            head = r;
        }
        tail = r;
        start_next_locked();
    }
    // Task-ul idle (tid 0) nu se poate bloca: la boot si in teste ramane pe interogare
    if(use_irq && irqs_on() && task_current()->tid != 0){
        r->done.wait();
        return r->status;
    }
    // Fara intreruperi (boot, teste): interogheaza hardware-ul. Starea se verifica sub lock, deci cand
    // done e setat, finish_locked a terminat de atins cererea si r poate disparea
    uint64_t end = monotonic_ns() + timeout_ms * 1000000;
    for(;;){
        {
            SpinlockGuard g(lock);
            if(r->done.is_done()){
                return r->status;
            }
            if(cur == r){
                ops.poll(ctx);
                if(r->done.is_done()){
                    return r->status;
                }
            }
            if(monotonic_ns() > end){
                if(cur == r){
                    ops.abort(ctx);
                    finish_locked(-EIO);
                } else {
                    BlockRequest** pp = &head;
                    BlockRequest* prev = nullptr;
                    while(*pp && *pp != r){
                        prev = *pp;
                        pp = &(*pp)->next;
                    }
                    if(*pp){
                        *pp = r->next;
                        if(tail == r){
                            tail = prev;
                        }
                    }
                    r->status = -EIO;
                    r->done.complete();
                }
                return r->status;
            }
        }
        __builtin_ia32_pause();
    }
}

int block_read_bytes(BlockDevice* dev, uint64_t pos, void* buf, uint64_t len){
    uint64_t bs = dev->block_size();
    uint64_t cap = bs * dev->block_count();
    if(pos > cap || len > cap - pos){
        return -EINVAL;
    }
    uint8_t* tmp = (uint8_t*)kmalloc(bs);
    if(!tmp){
        return -ENOMEM;
    }
    uint8_t* out = (uint8_t*)buf;
    int err = 0;
    while(len && !err){
        uint64_t lba = pos / bs;
        uint64_t off = pos % bs;
        uint64_t n = bs - off < len ? bs - off : len;
        if(off == 0 && n == bs){
            err = dev->read(lba, 1, out);
        } else {
            err = dev->read(lba, 1, tmp);
            if(!err){
                memcpy(out, tmp + off, n);
            }
        }
        pos += n;
        out += n;
        len -= n;
    }
    kfree(tmp);
    return err;
}

int block_write_bytes(BlockDevice* dev, uint64_t pos, const void* buf, uint64_t len){
    uint64_t bs = dev->block_size();
    uint64_t cap = bs * dev->block_count();
    if(pos > cap || len > cap - pos){
        return -EINVAL;
    }
    uint8_t* tmp = (uint8_t*)kmalloc(bs);
    if(!tmp){
        return -ENOMEM;
    }
    const uint8_t* in = (const uint8_t*)buf;
    int err = 0;
    while(len && !err){
        uint64_t lba = pos / bs;
        uint64_t off = pos % bs;
        uint64_t n = bs - off < len ? bs - off : len;
        if(off == 0 && n == bs){
            err = dev->write(lba, 1, in);
        } else {
            err = dev->read(lba, 1, tmp);
            if(!err){
                memcpy(tmp + off, in, n);
                err = dev->write(lba, 1, tmp);
            }
        }
        pos += n;
        in += n;
        len -= n;
    }
    kfree(tmp);
    return err;
}

namespace {

struct Out {
    char* buf;
    size_t n;
    size_t len;
    void put(char c){
        if(len + 1 < n){
            buf[len++] = c;
        }
    }
    void str(const char* s){
        while(*s){
            put(*s++);
        }
    }
    void dec(uint64_t v){
        char t[21];
        int i = 0;
        do {
            t[i++] = '0' + v % 10;
            v /= 10;
        } while(v);
        while(i){
            put(t[--i]);
        }
    }
    void pad(size_t col){
        while(len < col){
            put(' ');
        }
    }
    void done(){ buf[len < n ? len : n - 1] = 0; }
};

}

// "sda     disc 64 MiB, sector 512"  /  "sda1    part 32 MiB, start 2048, GPT, sda"
bool block_format(int idx, char* buf, size_t n){
    BlockDevice* d = hal_block(idx);
    if(!d || !n){
        return false;
    }
    Out o{buf, n, 0};
    o.str(hal_name(HalKind::Block, idx));
    o.pad(8);
    uint64_t bytes = d->block_count() * d->block_size();
    const PartitionDevice* p = partition_find(d);
    o.str(p ? "part " : "disc ");
    if(bytes >= (1UL << 20)){
        o.dec(bytes >> 20);
        o.str(" MiB");
    } else {
        o.dec(bytes >> 10);
        o.str(" KiB");
    }
    if(p){
        o.str(", start ");
        o.dec(p->start);
        o.str(p->info.gpt ? ", GPT" : ", MBR");
        if(!p->info.gpt){
            o.str(" tip 0x");
            const char* hex = "0123456789abcdef";
            o.put(hex[p->info.type >> 4]);
            o.put(hex[p->info.type & 15]);
        }
    } else {
        o.str(", sector ");
        o.dec(d->block_size());
    }
    o.done();
    return true;
}

void block_dump(){
    char line[96];
    for(int i = 0; block_format(i, line, sizeof(line)); i++){
        kprintf("%s\n", line);
    }
}

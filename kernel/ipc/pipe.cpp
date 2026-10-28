#include "ipc/pipe.h"
#include "uapi/syscall_nr.h"
#include "sync/spinlock.h"
#include "sync/waitqueue.h"
#include "mm/kmalloc.h"
#include "lib/new.h"
#include "lib/string.h"

namespace {

struct Pipe {
    Spinlock lock{"pipe"};
    WaitQueue readable;             // cititori care asteapta date
    WaitQueue writable;             // scriitori care asteapta loc
    uint32_t readers = 1;
    uint32_t writers = 1;
    uint64_t head = 0;              // urmatorul byte de citit
    uint64_t count = 0;
    uint8_t buf[PIPE_SIZE];
};

uint64_t min(uint64_t a, uint64_t b){
    return a < b ? a : b;
}

int64_t pipe_read(File* f, void* dst, uint64_t len, bool nonblock){
    Pipe* p = (Pipe*)f->priv;
    uint64_t flags = p->lock.lock_irqsave();
    while(p->count == 0){
        if(p->writers == 0 || nonblock){
            p->lock.unlock_irqrestore(flags);
            return p->writers == 0 ? 0 : -EAGAIN;
        }
        p->readable.wait(p->lock);
    }
    uint64_t n = min(len, p->count);
    uint8_t* d = (uint8_t*)dst;
    uint64_t first = min(n, PIPE_SIZE - p->head);
    memcpy(d, p->buf + p->head, first);
    memcpy(d + first, p->buf, n - first);
    p->head = (p->head + n) % PIPE_SIZE;
    p->count -= n;
    p->writable.wake_all();
    p->lock.unlock_irqrestore(flags);
    return (int64_t)n;
}

int64_t pipe_write(File* f, const void* src, uint64_t len, bool nonblock){
    Pipe* p = (Pipe*)f->priv;
    const uint8_t* s = (const uint8_t*)src;
    uint64_t done = 0;
    uint64_t flags = p->lock.lock_irqsave();
    while(done < len){
        if(p->readers == 0){
            p->lock.unlock_irqrestore(flags);
            return done ? (int64_t)done : -EPIPE;
        }
        if(p->count == PIPE_SIZE){
            if(nonblock){
                p->lock.unlock_irqrestore(flags);
                return done ? (int64_t)done : -EAGAIN;
            }
            p->writable.wait(p->lock);
            continue;
        }
        uint64_t n = min(len - done, PIPE_SIZE - p->count);
        uint64_t tail = (p->head + p->count) % PIPE_SIZE;
        uint64_t first = min(n, PIPE_SIZE - tail);
        memcpy(p->buf + tail, s + done, first);
        memcpy(p->buf, s + done + first, n - first);
        p->count += n;
        done += n;
        p->readable.wake_all();
    }
    p->lock.unlock_irqrestore(flags);
    return (int64_t)done;
}

int64_t no_read(File*, void*, uint64_t, bool){
    return -EBADF;
}

int64_t no_write(File*, const void*, uint64_t, bool){
    return -EBADF;
}

// Un capat inchis trezeste celalalt capat (EOF pentru cititori, EPIPE pentru scriitori)
void close_end(Pipe* p, bool reader){
    uint64_t flags = p->lock.lock_irqsave();
    if(reader){
        p->readers--;
        p->writable.wake_all();
    } else {
        p->writers--;
        p->readable.wake_all();
    }
    bool last = p->readers == 0 && p->writers == 0;
    p->lock.unlock_irqrestore(flags);
    if(last){
        p->~Pipe();
        kfree(p);
    }
}

void close_read(File* f){
    close_end((Pipe*)f->priv, true);
}

void close_write(File* f){
    close_end((Pipe*)f->priv, false);
}

const FileOps read_ops = {pipe_read, no_write, close_read};
const FileOps write_ops = {no_read, pipe_write, close_write};

}

int pipe_create(File*& rd, File*& wr){
    void* mem = kmalloc(sizeof(Pipe));
    if(!mem){
        return -ENOMEM;
    }
    Pipe* p = new (mem) Pipe();
    rd = file_alloc(&read_ops, p);
    wr = rd ? file_alloc(&write_ops, p) : nullptr;
    if(!wr){
        if(rd){
            kfree(rd);
        }
        p->~Pipe();
        kfree(p);
        return -ENOMEM;
    }
    return 0;
}

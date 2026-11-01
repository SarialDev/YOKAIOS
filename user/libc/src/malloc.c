/* malloc: liste libere peste brk (coalescing prin tag-uri de margine), blocuri mari direct prin mmap.
 * Chunk: [prev_size][size|flags][payload...]; prev_size e valid doar daca chunk-ul dinainte e liber
 * (altfel face parte din payload-ul lui). Utilizatorul primeste c + 16, aliniat la 16. */
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/mman.h>
#include "internal.h"

#define PINUSE 1UL
#define MMAPD 2UL
#define FLAGS 7UL
#define MIN_CHUNK 32UL
#define MMAP_THRESHOLD (128UL * 1024)
#define GROW 65536UL
#define TRIM_KEEP (256UL * 1024)

typedef struct chunk {
    size_t prev_size;
    size_t size;
    struct chunk *fd, *bk;      // doar cat timp e liber
} chunk;

static chunk free_list = {0, 0, &free_list, &free_list};
static chunk *top;              // ultimul chunk: ce a ramas din heap, nu e in lista
static char *heap_end;

static size_t csize(chunk *c){
    return c->size & ~FLAGS;
}

static chunk *next_chunk(chunk *c){
    return (chunk *)((char *)c + csize(c));
}

static void list_add(chunk *c){
    c->fd = free_list.fd;
    c->bk = &free_list;
    free_list.fd->bk = c;
    free_list.fd = c;
}

static void list_del(chunk *c){
    c->bk->fd = c->fd;
    c->fd->bk = c->bk;
}

static int heap_grow(size_t need){
    if(!top){
        char *b = __brk(0);
        size_t pad = (16 - ((size_t)b & 15)) & 15;
        if(pad && __brk(b + pad) != b + pad){
            return -1;
        }
        heap_end = b + pad;
    }
    size_t inc = (need + GROW - 1) & ~(GROW - 1);
    char *want = heap_end + inc;
    if(__brk(want) != want){
        return -1;
    }
    if(!top){
        top = (chunk *)heap_end;              // pagina exista abia acum
        top->size = PINUSE;
    }
    heap_end = want;
    top->size += inc;
    return 0;
}

static size_t request_size(size_t n){
    size_t s = (n + 8 + 15) & ~15UL;
    return s < MIN_CHUNK ? MIN_CHUNK : s;
}

void *malloc(size_t n){
    if(n > (1UL << 40)){
        errno = ENOMEM;
        return NULL;
    }
    size_t size = request_size(n);
    if(size >= MMAP_THRESHOLD){
        size_t len = (size + 8 + 4095) & ~4095UL;
        void *m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if(m == MAP_FAILED){
            return NULL;
        }
        chunk *c = m;
        c->prev_size = 0;
        c->size = len | MMAPD;
        return (char *)c + 16;
    }
    for(chunk *c = free_list.fd; c != &free_list; c = c->fd){
        size_t cs = csize(c);
        if(cs < size){
            continue;
        }
        list_del(c);
        if(cs - size >= MIN_CHUNK){
            chunk *r = (chunk *)((char *)c + size);
            r->size = (cs - size) | PINUSE;
            next_chunk(r)->prev_size = cs - size;
            list_add(r);
            c->size = size | PINUSE;
        } else {
            next_chunk(c)->size |= PINUSE;
        }
        return (char *)c + 16;
    }
    if(!top || csize(top) < size + MIN_CHUNK){
        if(heap_grow(size + MIN_CHUNK) < 0){
            errno = ENOMEM;
            return NULL;
        }
    }
    chunk *c = top;
    size_t rest = csize(top) - size;
    c->size = size | (c->size & PINUSE);
    top = (chunk *)((char *)c + size);
    top->size = rest | PINUSE;
    return (char *)c + 16;
}

void free(void *p){
    if(!p){
        return;
    }
    chunk *c = (chunk *)((char *)p - 16);
    if(c->size & MMAPD){
        munmap(c, csize(c));
        return;
    }
    size_t size = csize(c);
    if(!(c->size & PINUSE)){
        chunk *prev = (chunk *)((char *)c - c->prev_size);
        list_del(prev);
        size += csize(prev);
        c = prev;
    }
    chunk *next = (chunk *)((char *)c + size);
    if(next == top){
        c->size = (size + csize(top)) | PINUSE;
        top = c;
        // Intoarce kernelului ce depaseste TRIM_KEEP
        if(csize(top) > TRIM_KEEP + GROW){
            size_t cut = (csize(top) - TRIM_KEEP) & ~4095UL;
            if(cut && __brk(heap_end - cut) == heap_end - cut){
                heap_end -= cut;
                top->size -= cut;
            }
        }
        return;
    }
    if(!(next_chunk(next)->size & PINUSE)){
        list_del(next);
        size += csize(next);
        next = (chunk *)((char *)c + size);
    }
    c->size = size | PINUSE;
    next->prev_size = size;
    next->size &= ~PINUSE;
    list_add(c);
}

static size_t usable(chunk *c){
    return (c->size & MMAPD) ? csize(c) - 16 : csize(c) - 8;
}

void *calloc(size_t n, size_t m){
    if(m && n > (1UL << 40) / m){
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(n * m);
    if(p){
        chunk *c = (chunk *)((char *)p - 16);
        if(!(c->size & MMAPD)){
            memset(p, 0, n * m);
        }
    }
    return p;
}

void *realloc(void *p, size_t n){
    if(!p){
        return malloc(n);
    }
    if(n == 0){
        free(p);
        return NULL;
    }
    chunk *c = (chunk *)((char *)p - 16);
    size_t have = usable(c);
    if(n <= have && (!(c->size & MMAPD) || n > MMAP_THRESHOLD / 2)){
        return p;
    }
    void *q = malloc(n);
    if(q){
        memcpy(q, p, have < n ? have : n);
        free(p);
    }
    return q;
}

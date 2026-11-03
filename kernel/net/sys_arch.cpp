// Stratul de sistem al lwIP (NO_SYS=0) peste primitivele de kernel din T4: semafoare si mailbox-uri cu asteptare
// cu termen, mutex (cu priority inheritance), thread-uri de kernel si sectiunea critica globala.
// Un task care asteapta sta intr-un Waiter de pe stiva lui; cine semnaleaza scoate primul Waiter si cheama
// sched_wake sub lock-ul obiectului, deci un wake nu se pierde nici fata de expirarea termenului.
#include "lwip/opt.h"
#include "lwip/sys.h"
#include "lwip/err.h"
#include "sched/sched.h"
#include "sched/task.h"
#include "sync/spinlock.h"
#include "sync/mutex.h"
#include "sync/atomic.h"
#include "mm/kmalloc.h"
#include "lib/new.h"
#include "time/clock.h"

namespace {

struct Waiter {
    Waiter* prev;
    Waiter* next;
    Task* task;
    bool signaled;
};

struct WList {
    Waiter head;
};

void wl_init(WList& l){
    l.head.prev = l.head.next = &l.head;
}

bool wl_linked(const Waiter* w){
    return w->next != w;
}

void wl_remove(Waiter* w){
    if(wl_linked(w)){
        w->prev->next = w->next;
        w->next->prev = w->prev;
        w->prev = w->next = w;
    }
}

void wl_wake_one(WList& l){
    Waiter* w = l.head.next;
    if(w != &l.head){
        wl_remove(w);
        w->signaled = true;
        sched_wake(w->task);
    }
}

// Cu g luat (lock_irqsave facut de apelant): doarme pana la wake sau deadline_ns (0 = fara termen); g e luat la iesire
void wl_wait(Spinlock& g, WList& l, uint64_t deadline_ns){
    Task* t = task_current();
    Waiter w;
    w.prev = w.next = &w;
    w.task = t;
    w.signaled = false;
    w.prev = l.head.prev;
    w.next = &l.head;
    l.head.prev->next = &w;
    l.head.prev = &w;
    t->state = TaskState::BLOCKED;
    g.unlock();
    if(deadline_ns){
        sched_block_until(deadline_ns);
    } else {
        sched_block();
    }
    g.lock();
    wl_remove(&w);
}

uint64_t deadline_for(u32_t timeout_ms){
    return timeout_ms ? monotonic_ns() + (uint64_t)timeout_ms * 1000000UL : 0;
}

u32_t elapsed_ms(uint64_t t0){
    uint64_t d = (monotonic_ns() - t0) / 1000000UL;
    return d >= SYS_ARCH_TIMEOUT ? SYS_ARCH_TIMEOUT - 1 : (u32_t)d;
}

}

struct YkSem {
    Spinlock g;
    WList waiters;
    u32_t count;
    YkSem(u8_t initial) : g("lwip-sem"), count(initial){ wl_init(waiters); }
};

struct YkMutex {
    Mutex m;
    YkMutex() : m("lwip-mutex"){}
};

struct YkMbox {
    Spinlock g;
    WList readers;
    WList writers;
    void** slots;
    u32_t size, head, count;
    YkMbox(void** s, u32_t n) : g("lwip-mbox"), slots(s), size(n), head(0), count(0){
        wl_init(readers);
        wl_init(writers);
    }
};

extern "C" {

void sys_init(void){
}

u32_t sys_now(void){
    return (u32_t)(monotonic_ns() / 1000000UL);
}

u32_t sys_jiffies(void){
    return (u32_t)monotonic_ns();
}

// ---- sectiune critica globala: recursiva (lwIP o cuibareste), exclusiva intre CPU-uri si fata de IRQ ----

static volatile uint32_t prot_owner = ~0u;
static uint32_t prot_depth;

sys_prot_t sys_arch_protect(void){
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    uint32_t me = cpu_id();
    if(__atomic_load_n(&prot_owner, __ATOMIC_RELAXED) == me){
        prot_depth++;
    } else {
        for(;;){
            uint32_t expect = ~0u;
            if(__atomic_compare_exchange_n(&prot_owner, &expect, me, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)){
                break;
            }
            __builtin_ia32_pause();
        }
        prot_depth = 1;
    }
    return flags;
}

void sys_arch_unprotect(sys_prot_t flags){
    if(--prot_depth == 0){
        __atomic_store_n(&prot_owner, ~0u, __ATOMIC_RELEASE);
    }
    if(flags & (1UL << 9)){
        __asm__ volatile("sti" : : : "memory");
    }
}

// ---- semafoare ----

err_t sys_sem_new(sys_sem_t* sem, u8_t count){
    void* mem = kzalloc(sizeof(YkSem));
    if(!mem){
        *sem = NULL;
        return ERR_MEM;
    }
    *sem = new(mem) YkSem(count);
    return ERR_OK;
}

void sys_sem_free(sys_sem_t* sem){
    kfree(*sem);
    *sem = NULL;
}

void sys_sem_signal(sys_sem_t* sem){
    YkSem* s = *sem;
    uint64_t fl = s->g.lock_irqsave();
    s->count++;
    wl_wake_one(s->waiters);
    s->g.unlock_irqrestore(fl);
}

u32_t sys_arch_sem_wait(sys_sem_t* sem, u32_t timeout){
    YkSem* s = *sem;
    uint64_t t0 = monotonic_ns();
    uint64_t deadline = deadline_for(timeout);
    uint64_t fl = s->g.lock_irqsave();
    while(s->count == 0){
        if(deadline && monotonic_ns() >= deadline){
            // Un semnal pierdut de noi (expirat in acelasi timp cu el) trece la urmatorul asteptator
            if(s->count > 0){
                wl_wake_one(s->waiters);
            }
            s->g.unlock_irqrestore(fl);
            return SYS_ARCH_TIMEOUT;
        }
        wl_wait(s->g, s->waiters, deadline);
    }
    s->count--;
    if(s->count > 0){
        wl_wake_one(s->waiters);
    }
    s->g.unlock_irqrestore(fl);
    return elapsed_ms(t0);
}

// ---- mutex ----

err_t sys_mutex_new(sys_mutex_t* mutex){
    void* mem = kzalloc(sizeof(YkMutex));
    if(!mem){
        *mutex = NULL;
        return ERR_MEM;
    }
    *mutex = new(mem) YkMutex();
    return ERR_OK;
}

void sys_mutex_free(sys_mutex_t* mutex){
    kfree(*mutex);
    *mutex = NULL;
}

void sys_mutex_lock(sys_mutex_t* mutex){
    (*mutex)->m.lock();
}

void sys_mutex_unlock(sys_mutex_t* mutex){
    (*mutex)->m.unlock();
}

// ---- mailbox-uri ----

err_t sys_mbox_new(sys_mbox_t* mbox, int size){
    void* mem = kzalloc(sizeof(YkMbox));
    void** slots = (void**)kzalloc(sizeof(void*) * (size_t)size);
    if(!mem || !slots){
        kfree(mem);
        kfree(slots);
        *mbox = NULL;
        return ERR_MEM;
    }
    *mbox = new(mem) YkMbox(slots, (u32_t)size);
    return ERR_OK;
}

void sys_mbox_free(sys_mbox_t* mbox){
    YkMbox* m = *mbox;
    kfree(m->slots);
    kfree(m);
    *mbox = NULL;
}

static void mbox_put(YkMbox* m, void* msg){
    m->slots[(m->head + m->count) % m->size] = msg;
    m->count++;
    wl_wake_one(m->readers);
}

void sys_mbox_post(sys_mbox_t* mbox, void* msg){
    YkMbox* m = *mbox;
    uint64_t fl = m->g.lock_irqsave();
    while(m->count == m->size){
        wl_wait(m->g, m->writers, 0);
    }
    mbox_put(m, msg);
    m->g.unlock_irqrestore(fl);
}

err_t sys_mbox_trypost(sys_mbox_t* mbox, void* msg){
    YkMbox* m = *mbox;
    uint64_t fl = m->g.lock_irqsave();
    if(m->count == m->size){
        m->g.unlock_irqrestore(fl);
        return ERR_MEM;
    }
    mbox_put(m, msg);
    m->g.unlock_irqrestore(fl);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t* mbox, void* msg){
    return sys_mbox_trypost(mbox, msg);
}

static void* mbox_take(YkMbox* m){
    void* msg = m->slots[m->head];
    m->head = (m->head + 1) % m->size;
    m->count--;
    wl_wake_one(m->writers);
    if(m->count > 0){
        wl_wake_one(m->readers);
    }
    return msg;
}

u32_t sys_arch_mbox_fetch(sys_mbox_t* mbox, void** msg, u32_t timeout){
    YkMbox* m = *mbox;
    uint64_t t0 = monotonic_ns();
    uint64_t deadline = deadline_for(timeout);
    uint64_t fl = m->g.lock_irqsave();
    while(m->count == 0){
        if(deadline && monotonic_ns() >= deadline){
            m->g.unlock_irqrestore(fl);
            return SYS_ARCH_TIMEOUT;
        }
        wl_wait(m->g, m->readers, deadline);
    }
    void* got = mbox_take(m);
    if(msg){
        *msg = got;
    }
    m->g.unlock_irqrestore(fl);
    return elapsed_ms(t0);
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t* mbox, void** msg){
    YkMbox* m = *mbox;
    uint64_t fl = m->g.lock_irqsave();
    if(m->count == 0){
        m->g.unlock_irqrestore(fl);
        return SYS_MBOX_EMPTY;
    }
    void* got = mbox_take(m);
    if(msg){
        *msg = got;
    }
    m->g.unlock_irqrestore(fl);
    return 0;
}

// ---- thread-uri ----

sys_thread_t sys_thread_new(const char* name, lwip_thread_fn thread, void* arg, int stacksize, int prio){
    (void)stacksize;                            // stiva de kernel are dimensiune fixa (TASK_KSTACK_SIZE)
    Task* t = task_create_kernel(name, (TaskFn)thread, arg, (uint8_t)prio);
    if(!t){
        return NULL;
    }
    sched_add(t);
    return t;
}

}

#include "proc/process.h"
#include "uapi/syscall_nr.h"
#include "sched/sched.h"
#include "mm/kmalloc.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "lib/new.h"

namespace {

// Semnalul echivalent pentru codul de iesire al unui proces omorat (128 + semnal, ca in shell)
const int SIGILL = 4;
const int SIGFPE = 8;
const int SIGSEGV = 11;

struct ExitRecord {
    uint64_t pid;
    int code;
};

const int EXIT_HISTORY = 64;

Spinlock tree_lock("proc");
ExitRecord exits[EXIT_HISTORY];
uint64_t exit_count = 0;
uint64_t live_count = 0;

Process* sibling_of(ListLink* l){
    return (Process*)((uint8_t*)l - __builtin_offsetof(Process, sibling));
}

void link_add(ListLink& head, ListLink& l){
    l.prev = head.prev;
    l.next = &head;
    head.prev->next = &l;
    head.prev = &l;
}

void link_remove(ListLink& l){
    l.prev->next = l.next;
    l.next->prev = l.prev;
    l.prev = l.next = &l;
}


int kill_signal(uint64_t vector){
    switch(vector){
        case 0: return SIGFPE;      // #DE
        case 6: return SIGILL;      // #UD
        default: return SIGSEGV;
    }
}

[[noreturn]] void exit_with(int code, int status){
    Task* t = task_current();
    Process* p = t->proc;
    if(!p){
        panic("process_exit: task-ul %lu (%s) nu e un proces", t->tid, t->name);
    }
    // Inchiderea capetelor de pipe poate trezi alte procese, deci in afara lock-ului
    fd_close_all(p->fds);
    {
        uint64_t flags = tree_lock.lock_irqsave();
        exits[exit_count++ % EXIT_HISTORY] = ExitRecord{p->pid, code};
        tree_lock.unlock_irqrestore(flags);
    }
    kprintf("proces %lu (%s): exit %d\n", p->pid, p->name, code);

    // Copiii raman fara parinte: zombie-ii deja eliberati de task se elibereaza acum, restul la task_destroy
    ListLink orphans = {&orphans, &orphans};
    uint64_t flags = tree_lock.lock_irqsave();
    while(p->children.next != &p->children){
        Process* c = sibling_of(p->children.next);
        link_remove(c->sibling);
        c->parent = nullptr;
        c->ppid = 0;
        if(c->state == ProcState::ZOMBIE && !c->main){
            link_add(orphans, c->sibling);
        }
    }
    p->exit_code = code;
    p->wait_status = status;
    p->state = ProcState::ZOMBIE;
    if(p->parent){
        p->parent->child_exit.wake_all();
    }
    tree_lock.unlock_irqrestore(flags);
    while(orphans.next != &orphans){
        Process* c = sibling_of(orphans.next);
        link_remove(c->sibling);
        process_free(c);
    }
    task_exit();
}

}

Process* process_create(Task* main, Process* parent){
    void* mem = kzalloc(sizeof(Process));
    if(!mem){
        return nullptr;
    }
    Process* p = new (mem) Process();
    p->pid = main->tid;
    p->state = ProcState::ALIVE;
    p->main = main;
    p->children.prev = p->children.next = &p->children;
    p->sibling.prev = p->sibling.next = &p->sibling;
    process_set_name(p, main->name);
    main->pid = p->pid;
    main->proc = p;
    __atomic_fetch_add(&live_count, 1, __ATOMIC_RELAXED);
    if(parent){
        uint64_t flags = tree_lock.lock_irqsave();
        p->parent = parent;
        p->ppid = parent->pid;
        link_add(parent->children, p->sibling);
        tree_lock.unlock_irqrestore(flags);
    }
    return p;
}

void process_abort(Process* p){
    if(p->parent){
        uint64_t flags = tree_lock.lock_irqsave();
        link_remove(p->sibling);
        tree_lock.unlock_irqrestore(flags);
    }
    fd_close_all(p->fds);
    if(p->main){
        p->main->proc = nullptr;
    }
    process_free(p);
}

void process_free(Process* p){
    p->~Process();
    kfree(p);
    __atomic_fetch_sub(&live_count, 1, __ATOMIC_RELAXED);
}

Process* process_current(){
    return task_current()->proc;
}

void process_set_name(Process* p, const char* name){
    int i = 0;
    for(; i < TASK_NAME_LEN - 1 && name[i]; i++){
        p->name[i] = name[i];
    }
    p->name[i] = 0;
}

void process_exit(int code){
    exit_with(code, W_EXITCODE(code));
}

void process_kill(InterruptFrame* f){
    Task* t = task_current();
    kprintf("proces %lu (%s) omorat: %s la rip=%lx\n", t->pid, t->name, exception_name(f->vector), f->rip);
    int sig = kill_signal(f->vector);
    exit_with(128 + sig, sig);
}

void process_task_destroyed(Task* t){
    Process* p = t->proc;
    if(!p){
        return;
    }
    uint64_t flags = tree_lock.lock_irqsave();
    p->main = nullptr;
    t->proc = nullptr;
    bool done = p->reaped || (p->state == ProcState::ZOMBIE && !p->parent);
    tree_lock.unlock_irqrestore(flags);
    if(done){
        process_free(p);
    }
}

bool process_exit_status(uint64_t pid, int& code){
    SpinlockGuard lock(tree_lock);
    for(int i = 0; i < EXIT_HISTORY; i++){
        if(exits[i].pid == pid){
            code = exits[i].code;
            return true;
        }
    }
    return false;
}

uint64_t process_count(){
    return __atomic_load_n(&live_count, __ATOMIC_RELAXED);
}

Spinlock& process_tree_lock(){
    return tree_lock;
}

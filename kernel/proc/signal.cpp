#include "proc/signal.h"
#include "sync/pollwait.h"
#include "arch/x86_64/fpu.h"
#include "proc/process.h"
#include "syscall/syscall.h"
#include "sched/sched.h"
#include "sync/waitqueue.h"
#include "mm/pmm.h"
#include "mm/pageref.h"
#include "mm/vmm.h"
#include "core/panic.h"
#include "time/clock.h"
#include "lib/kprintf.h"
#include "lib/string.h"

namespace {

const uint64_t RFLAGS_USER_MASK = 0xCD5;        // CF PF AF ZF SF DF OF: singurele pe care le poate schimba sigreturn
const uint64_t RFLAGS_TF = 1UL << 8;
const uint64_t RFLAGS_DF = 1UL << 10;
const uint64_t RED_ZONE = 128;

inline uint64_t bit(int sig){
    return 1UL << (sig - 1);
}

const uint64_t UNBLOCKABLE = (1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1));

// Actiunea implicita "ignora". Oprirea (STOP / TSTP / TTIN / TTOU) nu exista: se ignora
bool default_ignored(int sig){
    switch(sig){
        case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH:
        case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
            return true;
        default:
            return false;
    }
}

struct SigInfo {
    int32_t signo;
    int32_t err;
    int32_t code;
    int32_t pad;
    uint64_t addr;                              // si_addr: adresa faultului
    int64_t pid;                                // si_pid (doar kill)
    uint8_t rest[128 - 32];
};
static_assert(sizeof(SigInfo) == 128, "siginfo are 128 de bytes");

struct UserRegs {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t rip, rflags, rsp;
};

struct UContext {
    uint64_t flags;
    uint64_t mask;                              // masca de restaurat la sigreturn
    UserRegs regs;
    uint8_t fpstate[FPU_STATE_SIZE];            // FXSAVE al codului intrerupt (nealiniat: se copiaza prin tampon)
};

// Ce se pune pe stiva user cand intra un handler; [rsp] = adresa de intoarcere (trambulina), ca dupa un call
struct SigFrame {
    uint64_t ret;
    SigInfo info;
    UContext uc;
};

Spinlock tramp_lock("sigtramp");
uint64_t tramp_phys = 0;

uint64_t trampoline_page(){
    SpinlockGuard lock(tramp_lock);
    if(!tramp_phys){
        uint64_t phys = pmm_alloc_page();
        if(!phys){
            return 0;
        }
        uint8_t* p = (uint8_t*)phys_to_virt(phys);
        memset(p, 0xCC, PAGE_SIZE);
        // mov $SYS_RT_SIGRETURN, %eax ; syscall ; ud2
        const uint8_t code[] = {0xB8, SYS_RT_SIGRETURN, 0, 0, 0, 0x0F, 0x05, 0x0F, 0x0B};
        memcpy(p, code, sizeof(code));
        tramp_phys = phys;                      // referinta kernelului: pagina nu se elibereaza niciodata
    }
    return tramp_phys;
}

int signal_for_vector(uint64_t vec){
    switch(vec){
        case 0: case 16: case 19: return SIGFPE;
        case 1: case 3: return SIGTRAP;
        case 6: return SIGILL;
        case 17: return SIGBUS;
        default: return SIGSEGV;
    }
}

bool sig_valid(uint64_t sig){
    return sig >= 1 && sig < (uint64_t)NSIG;
}

// Pune handlerul sig pe stiva user si schimba frame-ul ca sa sara in el. false daca stiva user nu e scriibila
bool push_frame(InterruptFrame* f, Process* p, int sig, const KSigAction& a, int64_t nr, const SigInfo& info){
    if(nr >= 0 && (int64_t)f->rax == -EINTR && (a.flags & SA_RESTART) &&
       (nr == SYS_READ || nr == SYS_WRITE || nr == SYS_WAIT4)){
        f->rip -= 2;                            // sysret/syscall = 2 bytes: dupa handler, syscall-ul se reia
        f->rax = (uint64_t)nr;
    }
    SigFrame sf;
    memset(&sf, 0, sizeof(sf));
    uint64_t sp = (f->rsp - RED_ZONE - sizeof(SigFrame)) & ~15UL;
    sp -= 8;                                    // dupa "ret", rsp + 8 e aliniat la 16, ca la intrarea intr-o functie
    sf.ret = (a.flags & SA_RESTORER) && a.restorer ? a.restorer : SIGTRAMP_ADDR;
    sf.info = info;
    sf.uc.mask = p->sig.has_saved ? p->sig.saved_mask : p->sig.blocked;
    alignas(16) uint8_t fx[FPU_STATE_SIZE];
    fpu_save(fx);
    memcpy(sf.uc.fpstate, fx, sizeof(fx));
    sf.uc.regs = UserRegs{f->r15, f->r14, f->r13, f->r12, f->r11, f->r10, f->r9, f->r8, f->rbp, f->rdi, f->rsi,
                          f->rdx, f->rcx, f->rbx, f->rax, f->rip, f->rflags, f->rsp};
    // Pagini de stiva noi (page fault) se aduc cu intreruperile pornite
    __asm__ volatile("sti");
    bool ok = copy_to_user(sp, &sf, sizeof(sf));
    __asm__ volatile("cli");
    if(!ok){
        return false;
    }
    f->rip = a.handler;
    f->rsp = sp;
    f->rdi = (uint64_t)sig;
    f->rsi = sp + __builtin_offsetof(SigFrame, info);
    f->rdx = sp + __builtin_offsetof(SigFrame, uc);
    f->rax = 0;
    f->rflags &= ~(RFLAGS_DF | RFLAGS_TF);
    p->sig.blocked |= a.mask & ~UNBLOCKABLE;
    if(!(a.flags & SA_NODEFER)){
        p->sig.blocked |= bit(sig);
    }
    p->sig.has_saved = false;
    if(a.flags & SA_RESETHAND){
        memset(&p->sig.act[sig], 0, sizeof(KSigAction));
    }
    return true;
}

struct SendReq {
    int sig;
    int result;
    uint64_t pgid;
    int count;
    bool cap_kill;                  // expeditorul are CAP_KILL
    const Cred* from;               // cine trimite (copie: procesul curent poate fi chiar tinta)
    int denied;                     // procese din grup sarite pentru ca from nu are dreptul
};

void visit_pid(Task* t, void* arg){
    SendReq* r = (SendReq*)arg;
    SpinlockGuard lock(process_tree_lock());
    Process* p = t->proc;
    if(!p){
        r->result = -EPERM;
    } else if(p->state != ProcState::ALIVE){
        r->result = -ESRCH;
    } else if(!cred_may_signal(*r->from, r->cap_kill, p->cred)){
        r->result = -EPERM;
    } else {
        signal_post_locked(p, r->sig);
        r->result = 0;
    }
}

void visit_pgrp(Task* t, void* arg){
    SendReq* r = (SendReq*)arg;
    SpinlockGuard lock(process_tree_lock());
    Process* p = t->proc;
    if(p && p->pgid == r->pgid && p->state == ProcState::ALIVE){
        if(!cred_may_signal(*r->from, r->cap_kill, p->cred)){
            r->denied++;
            return;
        }
        signal_post_locked(p, r->sig);
        r->count++;
    }
}

}

bool signal_pending(){
    Process* p = process_current();
    return p && (__atomic_load_n(&p->sig.pending, __ATOMIC_ACQUIRE) & ~p->sig.blocked);
}

void signal_fork(Process* child, const Process* parent){
    memcpy(child->sig.act, parent->sig.act, sizeof(child->sig.act));
    child->sig.blocked = parent->sig.blocked;
    child->sig.pending = 0;
}

void signal_exec(Process* p){
    for(int s = 1; s < NSIG; s++){
        if(p->sig.act[s].handler > SIG_IGN){
            memset(&p->sig.act[s], 0, sizeof(KSigAction));
        }
    }
    p->sig.has_saved = false;
}

bool signal_map_trampoline(AddressSpace& as){
    uint64_t phys = trampoline_page();
    if(!phys || !vmm_map(as, SIGTRAMP_ADDR, phys, PTE_USER)){
        return false;
    }
    page_ref(phys);
    return true;
}

void signal_post_locked(Process* p, int sig){
    if(sig <= 0 || sig >= NSIG || p->state != ProcState::ALIVE){
        return;
    }
    const KSigAction& a = p->sig.act[sig];
    if(sig != SIGKILL && (a.handler == SIG_IGN || (a.handler == SIG_DFL && default_ignored(sig)))){
        return;                                 // semnalele ignorate nu raman pending
    }
    __atomic_fetch_or(&p->sig.pending, bit(sig), __ATOMIC_ACQ_REL);
    if(p->main){
        WaitQueue::interrupt_task(p->main);
        poll_interrupt(p->main);
    }
}

int signal_send_pid(uint64_t pid, int sig){
    if(sig < 0 || sig >= NSIG){
        return -EINVAL;
    }
    Cred from = cred_current();
    SendReq req = {sig, -ESRCH, 0, 0, capable(CAP_KILL), &from, 0};
    if(!task_visit(pid, visit_pid, &req)){
        return -ESRCH;
    }
    return req.result;
}

int signal_send_pgrp(uint64_t pgid, int sig, bool check_perm){
    if(sig < 0 || sig >= NSIG){
        return -EINVAL;
    }
    Cred from = cred_current();
    bool cap_kill = capable(CAP_KILL);
    if(!check_perm){
        from = cred_root;               // terminalul (Ctrl-C) trimite oricui
        cap_kill = true;
    }
    SendReq req = {sig, 0, pgid, 0, cap_kill, &from, 0};
    task_for_each(visit_pgrp, &req);
    return req.count ? 0 : req.denied ? -EPERM : -ESRCH;
}

void signal_send_self(int sig){
    Process* p = process_current();
    if(p){
        SpinlockGuard lock(process_tree_lock());
        signal_post_locked(p, sig);
    }
}

void signal_deliver(InterruptFrame* f, int64_t syscall_nr){
    Process* p = process_current();
    if(!p){
        return;
    }
    bool delivered = false;
    for(;;){
        uint64_t ready = __atomic_load_n(&p->sig.pending, __ATOMIC_ACQUIRE) & ~p->sig.blocked;
        if(!ready){
            break;
        }
        int sig = __builtin_ctzl(ready) + 1;
        __atomic_fetch_and(&p->sig.pending, ~bit(sig), __ATOMIC_ACQ_REL);
        KSigAction a = p->sig.act[sig];
        if(sig == SIGKILL || a.handler == SIG_DFL){
            if(sig != SIGKILL && default_ignored(sig)){
                continue;
            }
            process_die(sig);
        }
        if(a.handler == SIG_IGN){
            continue;
        }
        SigInfo info;
        memset(&info, 0, sizeof(info));
        info.signo = sig;
        info.code = SI_USER;
        if(!push_frame(f, p, sig, a, syscall_nr, info)){
            kprintf("proces %lu (%s): stiva user invalida la livrarea semnalului %d\n", p->pid, p->name, sig);
            process_die(SIGSEGV);
        }
        delivered = true;
        break;
    }
    if(!delivered && p->sig.has_saved){
        p->sig.blocked = p->sig.saved_mask;
        p->sig.has_saved = false;
    }
}

void signal_fault(InterruptFrame* f, uint64_t addr){
    Process* p = process_current();
    int sig = signal_for_vector(f->vector);
    if(p){
        const KSigAction a = p->sig.act[sig];
        if(a.handler > SIG_IGN && !(p->sig.blocked & bit(sig))){
            SigInfo info;
            memset(&info, 0, sizeof(info));
            info.signo = sig;
            info.addr = addr;
            if(sig == SIGSEGV){
                info.code = (f->error & 1) ? SEGV_ACCERR : SEGV_MAPERR;
            } else if(sig == SIGFPE){
                info.code = FPE_INTDIV;
            } else if(sig == SIGILL){
                info.code = ILL_ILLOPC;
            }
            if(push_frame(f, p, sig, a, -1, info)){
                return;
            }
        }
        kprintf("proces %lu (%s) omorat: %s la rip=%lx\n", p->pid, p->name, exception_name(f->vector), f->rip);
        process_die(sig);
    }
}

int64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oact, uint64_t size, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p || size != sizeof(uint64_t) || !sig_valid(sig)){
        return -EINVAL;
    }
    int s = (int)sig;
    KSigAction na;
    if(act){
        if(s == SIGKILL || s == SIGSTOP){
            return -EINVAL;
        }
        if(!copy_from_user(&na, act, sizeof(na))){
            return -EFAULT;
        }
        if(na.handler > SIG_IGN && na.handler >= USER_TOP){
            return -EFAULT;
        }
        if((na.flags & SA_RESTORER) && na.restorer >= USER_TOP){
            return -EFAULT;
        }
        na.mask &= ~UNBLOCKABLE;
    }
    if(oact && !copy_to_user(oact, &p->sig.act[s], sizeof(KSigAction))){
        return -EFAULT;
    }
    if(act){
        SpinlockGuard lock(process_tree_lock());
        p->sig.act[s] = na;
        if(na.handler == SIG_IGN || (na.handler == SIG_DFL && default_ignored(s))){
            __atomic_fetch_and(&p->sig.pending, ~bit(s), __ATOMIC_ACQ_REL);
        }
    }
    return 0;
}

int64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oset, uint64_t size, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p || size != sizeof(uint64_t)){
        return -EINVAL;
    }
    uint64_t old = p->sig.blocked;
    if(set){
        uint64_t m;
        if(!copy_from_user(&m, set, sizeof(m))){
            return -EFAULT;
        }
        m &= ~UNBLOCKABLE;
        switch(how){
            case SIG_BLOCK: p->sig.blocked = old | m; break;
            case SIG_UNBLOCK: p->sig.blocked = old & ~m; break;
            case SIG_SETMASK: p->sig.blocked = m; break;
            default: return -EINVAL;
        }
    }
    if(oset && !copy_to_user(oset, &old, sizeof(old))){
        return -EFAULT;
    }
    return 0;
}

int64_t sys_rt_sigreturn(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    InterruptFrame* f = syscall_frame();
    if(!p){
        return -EINVAL;
    }
    // Trambulina a facut "ret" din handler: rsp indica imediat dupa adresa de intoarcere din SigFrame
    uint64_t base = f->rsp - sizeof(uint64_t);
    UContext uc;
    if(!copy_from_user(&uc, base + __builtin_offsetof(SigFrame, uc), sizeof(uc)) || uc.regs.rip >= USER_TOP ||
       uc.regs.rsp >= USER_TOP){
        kprintf("proces %lu (%s): sigreturn cu frame invalid\n", p->pid, p->name);
        process_die(SIGSEGV);
    }
    alignas(16) uint8_t fx[FPU_STATE_SIZE];
    memcpy(fx, uc.fpstate, sizeof(fx));
    *(uint32_t*)(fx + 24) &= 0xFFBF;            // MXCSR: bitii rezervati dau #GP in kernel la fxrstor
    fpu_restore(fx);
    const UserRegs& r = uc.regs;
    f->r15 = r.r15; f->r14 = r.r14; f->r13 = r.r13; f->r12 = r.r12; f->r11 = r.r11; f->r10 = r.r10;
    f->r9 = r.r9; f->r8 = r.r8; f->rbp = r.rbp; f->rdi = r.rdi; f->rsi = r.rsi; f->rdx = r.rdx;
    f->rcx = r.rcx; f->rbx = r.rbx;
    f->rip = r.rip;
    f->rsp = r.rsp;
    f->rflags = (r.rflags & RFLAGS_USER_MASK) | 0x202;
    p->sig.blocked = uc.mask & ~UNBLOCKABLE;
    return (int64_t)r.rax;                      // syscall_dispatch il pune in rax
}

int64_t sys_rt_sigpending(uint64_t set, uint64_t size, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p || size != sizeof(uint64_t)){
        return -EINVAL;
    }
    uint64_t m = __atomic_load_n(&p->sig.pending, __ATOMIC_ACQUIRE) & p->sig.blocked;
    return copy_to_user(set, &m, sizeof(m)) ? 0 : -EFAULT;
}

namespace {

// Asteapta (felii de 10 ms) pana apare un semnal de livrat
void wait_for_signal(){
    while(!signal_pending()){
        if(!sched_sleep_until(monotonic_ns() + 10000000UL)){
            yield();
        }
    }
}

}

int64_t sys_rt_sigsuspend(uint64_t set, uint64_t size, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p || size != sizeof(uint64_t)){
        return -EINVAL;
    }
    uint64_t m;
    if(!copy_from_user(&m, set, sizeof(m))){
        return -EFAULT;
    }
    p->sig.saved_mask = p->sig.blocked;
    p->sig.has_saved = true;
    p->sig.blocked = m & ~UNBLOCKABLE;
    wait_for_signal();
    return -EINTR;
}

int64_t sys_pause(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    wait_for_signal();
    return -EINTR;
}

namespace {

struct PgReq {
    uint64_t caller;
    int64_t result;
    uint64_t pgid;
    bool set;
};

void visit_pgid(Task* t, void* arg){
    PgReq* r = (PgReq*)arg;
    SpinlockGuard lock(process_tree_lock());
    Process* p = t->proc;
    if(!p || p->state != ProcState::ALIVE){
        r->result = -ESRCH;
    } else if(r->set){
        // Doar procesul insusi sau un copil al apelantului
        if(p->pid != r->caller && p->ppid != r->caller){
            r->result = -EPERM;
        } else {
            p->pgid = r->pgid ? r->pgid : p->pid;
            r->result = 0;
        }
    } else {
        r->result = (int64_t)p->pgid;
    }
}

}

int64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p || (int64_t)pgid < 0){
        return -EINVAL;
    }
    PgReq req = {p->pid, -ESRCH, pgid, true};
    if(!task_visit(pid ? pid : p->pid, visit_pgid, &req)){
        return -ESRCH;
    }
    return req.result;
}

int64_t sys_getpgrp(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    return p ? (int64_t)p->pgid : -EINVAL;
}

int64_t sys_getpgid(uint64_t pid, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t){
    Process* p = process_current();
    if(!p){
        return -EINVAL;
    }
    PgReq req = {p->pid, -ESRCH, 0, false};
    if(!task_visit(pid ? pid : p->pid, visit_pgid, &req)){
        return -ESRCH;
    }
    return req.result;
}

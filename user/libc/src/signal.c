/* Semnale: sigaction peste rt_sigaction (struct kernel_sigaction), multimi de semnale, raise, abort */
#include <signal.h>
#include <errno.h>
#include <unistd.h>

struct ksigaction {
    unsigned long handler;
    unsigned long flags;
    unsigned long restorer;
    unsigned long mask;
};

int rt_sigaction(int sig, const struct ksigaction *act, struct ksigaction *oact, unsigned long size);
int rt_sigprocmask(int how, const sigset_t *set, sigset_t *oset, unsigned long size);
int rt_sigpending(sigset_t *set, unsigned long size);
int rt_sigsuspend(const sigset_t *mask, unsigned long size);

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact){
    struct ksigaction ka, ko;
    if(act){
        ka.handler = (unsigned long)act->sa_handler;
        ka.flags = (unsigned long)(unsigned)act->sa_flags;
        ka.restorer = 0;                    /* trambulina kernelului */
        ka.mask = act->sa_mask;
    }
    if(rt_sigaction(sig, act ? &ka : 0, oact ? &ko : 0, sizeof(sigset_t)) < 0){
        return -1;
    }
    if(oact){
        oact->sa_handler = (void (*)(int))ko.handler;
        oact->sa_flags = (int)ko.flags;
        oact->sa_mask = ko.mask;
    }
    return 0;
}

sighandler_t signal(int sig, sighandler_t handler){
    struct sigaction sa, old;
    sa.sa_handler = handler;
    sa.sa_mask = 0;
    sa.sa_flags = SA_RESTART;
    if(sigaction(sig, &sa, &old) < 0){
        return SIG_ERR;
    }
    return old.sa_handler;
}

int sigprocmask(int how, const sigset_t *set, sigset_t *oset){
    return rt_sigprocmask(how, set, oset, sizeof(sigset_t));
}

int sigpending(sigset_t *set){
    return rt_sigpending(set, sizeof(sigset_t));
}

int sigsuspend(const sigset_t *mask){
    return rt_sigsuspend(mask, sizeof(sigset_t));
}

static int bad(int sig){
    if(sig <= 0 || sig >= NSIG){
        errno = EINVAL;
        return 1;
    }
    return 0;
}

int sigemptyset(sigset_t *set){
    *set = 0;
    return 0;
}

int sigfillset(sigset_t *set){
    *set = ~0UL >> (64 - (NSIG - 1));
    return 0;
}

int sigaddset(sigset_t *set, int sig){
    if(bad(sig)){
        return -1;
    }
    *set |= 1UL << (sig - 1);
    return 0;
}

int sigdelset(sigset_t *set, int sig){
    if(bad(sig)){
        return -1;
    }
    *set &= ~(1UL << (sig - 1));
    return 0;
}

int sigismember(const sigset_t *set, int sig){
    if(bad(sig)){
        return -1;
    }
    return (*set >> (sig - 1)) & 1;
}

int raise(int sig){
    return kill(getpid(), sig);
}

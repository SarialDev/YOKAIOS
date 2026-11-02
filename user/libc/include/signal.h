#ifndef _SIGNAL_H
#define _SIGNAL_H
#include <sys/types.h>
/* Semnale POSIX (numerele de pe Linux x86-64; 1..31). Handlerele ruleaza pe stiva programului; la intoarcere
 * kernelul reface registrele (sigreturn prin trambulina lui). Oprirea (SIGSTOP/SIGTSTP) nu exista: se ignora */
#define SIGHUP 1
#define SIGINT 2
#define SIGQUIT 3
#define SIGILL 4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS 7
#define SIGFPE 8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG 23
#define SIGWINCH 28
#define SIGSYS 31
#define NSIG 32

typedef unsigned long sigset_t;
typedef int sig_atomic_t;
typedef void (*sighandler_t)(int);

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)
#define SIG_ERR ((sighandler_t)-1)

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO 0x00000004
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000

#define SIG_BLOCK 0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SI_USER 0
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2

typedef struct {
    int si_signo;
    int si_errno;
    int si_code;
    int __pad;
    void *si_addr;
    pid_t si_pid;
    char __rest[128 - 36];
} siginfo_t;

struct sigaction {
    union {
        void (*sa_handler)(int);
        void (*sa_sigaction)(int, siginfo_t *, void *);
    };
    sigset_t sa_mask;
    int sa_flags;
};

int kill(pid_t pid, int sig);
int raise(int sig);
sighandler_t signal(int sig, sighandler_t handler);
int sigaction(int sig, const struct sigaction *act, struct sigaction *oact);
int sigprocmask(int how, const sigset_t *set, sigset_t *oset);
int sigpending(sigset_t *set);
int sigsuspend(const sigset_t *mask);
int sigemptyset(sigset_t *set);
int sigfillset(sigset_t *set);
int sigaddset(sigset_t *set, int sig);
int sigdelset(sigset_t *set, int sig);
int sigismember(const sigset_t *set, int sig);
#endif

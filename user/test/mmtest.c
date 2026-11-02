/* Teste T8: mmap / munmap / mprotect / msync / brk, fork cu CoW (64 MiB), semnale (sigaction, masca, sigreturn,
 * SA_RESTART, EINTR, SIGSEGV/SIGPIPE/SIGCHLD/SIGKILL, grupuri), Ctrl-C (kernelul tasteaza 0x03 cand un copil e in
 * prim-plan). Pornit de kernel cu test=mm (kernel/tests/test_mm.cpp). Iesire 0 = totul bine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "mmtest: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while(0)

#define MiB (1024UL * 1024UL)
#define PAGE 4096UL

/* MemFree din /proc/meminfo, in KiB */
static long mem_free_kb(void){
    int fd = open("/proc/meminfo", O_RDONLY);
    char buf[512];
    ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if(fd >= 0){
        close(fd);
    }
    if(n <= 0){
        return -1;
    }
    buf[n] = 0;
    char *p = strstr(buf, "MemFree:");
    return p ? strtol(p + 8, NULL, 10) : -1;
}

/* ---------------- SIGSEGV prins cu longjmp ---------------- */

static jmp_buf jb;
static volatile int got_sig;
static volatile void *fault_addr;
static volatile int fault_code;

static void on_fault(int sig, siginfo_t *si, void *uc){
    (void)uc;
    got_sig = sig;
    fault_addr = si->si_addr;
    fault_code = si->si_code;
    longjmp(jb, 1);
}

static void unblock(int sig){
    sigset_t m = 0;
    sigaddset(&m, sig);
    sigprocmask(SIG_UNBLOCK, &m, NULL);
}

/* 1 daca scrierea (write = 1) sau citirea la p produce SIGSEGV */
static int faults(volatile char *p, int write){
    got_sig = 0;
    if(setjmp(jb) == 0){
        if(write){
            *p = 1;
        } else {
            (void)*p;
        }
        return 0;
    }
    unblock(SIGSEGV);                   /* longjmp iese din handler fara sigreturn */
    return got_sig == SIGSEGV && fault_addr == (void *)p;
}

static void install(int sig, void (*h)(int, siginfo_t *, void *), int flags){
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = h;
    sa.sa_flags = SA_SIGINFO | flags;
    CHECK(sigaction(sig, &sa, NULL) == 0);
}

static void set_handler(int sig, void (*h)(int), int flags){
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = h;
    sa.sa_flags = flags;
    CHECK(sigaction(sig, &sa, NULL) == 0);
}

/* ---------------- mmap anonim, munmap, mprotect, brk ---------------- */

static void test_anon(void){
    long f0 = mem_free_kb();
    size_t len = 16 * MiB;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    long f1 = mem_free_kb();
    CHECK(f0 - f1 < 512);               /* paginile vin la primul acces */
    for(size_t i = 0; i < len; i += PAGE){
        CHECK(p[i] == 0);
        p[i] = (char)(i >> 12);
    }
    long f2 = mem_free_kb();
    CHECK(f1 - f2 > 15 * 1024);
    CHECK(p[5 * PAGE] == 5);
    /* munmap la mijloc: gaura prinde SIGSEGV, restul ramane */
    CHECK(munmap(p + 4 * PAGE, 2 * PAGE) == 0);
    install(SIGSEGV, on_fault, 0);
    CHECK(faults(p + 4 * PAGE, 0) && fault_code == SEGV_MAPERR);
    CHECK(faults(p + 5 * PAGE + 7, 1));
    CHECK(p[6 * PAGE] == 6 && p[3 * PAGE] == 3);
    /* MAP_FIXED umple gaura cu pagini zero */
    char *q = mmap(p + 4 * PAGE, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(q == p + 4 * PAGE && q[0] == 0 && q[PAGE] == 0);
    /* mprotect: RO -> scrierea da SEGV_ACCERR, citirea merge; PROT_NONE -> si citirea; inapoi RW */
    CHECK(mprotect(p, 2 * PAGE, PROT_READ) == 0);
    CHECK(p[PAGE] == 1);
    CHECK(faults(p + PAGE, 1) && fault_code == SEGV_ACCERR);
    CHECK(mprotect(p, PAGE, PROT_NONE) == 0);
    CHECK(faults(p, 0));
    CHECK(mprotect(p, 2 * PAGE, PROT_READ | PROT_WRITE) == 0);
    p[PAGE] = 9;
    CHECK(p[PAGE] == 9 && p[0] == 0);
    CHECK(mprotect(p + len, PAGE, PROT_READ) < 0 && errno == ENOMEM);     /* in afara zonei */
    /* Kernelul scrie in pagini neaduse inca (read() intr-un buffer mmap proaspat) */
    char *r = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int fd = open("/proc/meminfo", O_RDONLY);
    CHECK(fd >= 0 && read(fd, r, 64) == 64 && memcmp(r, "MemTotal", 8) == 0);
    close(fd);
    /* Erori */
    CHECK(mmap(NULL, 0, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL);
    CHECK(mmap(NULL, PAGE, PROT_READ, MAP_ANONYMOUS, -1, 0) == MAP_FAILED && errno == EINVAL);
    CHECK(mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, 77, 0) == MAP_FAILED && errno == EBADF);
    CHECK(munmap(p + 1, PAGE) < 0 && errno == EINVAL);
    CHECK(munmap(r, PAGE) == 0 && munmap(p, len) == 0);
    long f3 = mem_free_kb();
    CHECK(f3 > f2 + 15 * 1024);
    /* brk: creste lent, se micsoreaza */
    char *b0 = sbrk(0);
    CHECK(sbrk(8 * MiB) == b0);
    CHECK(b0[0] == 0 && b0[8 * MiB - 1] == 0);
    b0[MiB] = 'x';
    CHECK(sbrk(-(long)(8 * MiB)) == b0 + 8 * MiB && sbrk(0) == b0);
    CHECK(faults(b0 + MiB, 0));
    /* Stiva creste la cerere peste cei 64 KiB initiali */
    volatile char big[256 * 1024];
    big[0] = 1;
    big[sizeof(big) - 1] = 2;
    CHECK(big[0] + big[sizeof(big) - 1] == 3);
    signal(SIGSEGV, SIG_DFL);
}

/* Handlerul repara faultul (mprotect) si se intoarce: instructiunea se reia si reuseste */
static char *fix_page;
static volatile int fixes;
static void on_fault_fix(int sig, siginfo_t *si, void *uc){
    (void)sig;
    (void)uc;
    fixes++;
    mprotect((void *)((unsigned long)si->si_addr & ~(PAGE - 1)), PAGE, PROT_READ | PROT_WRITE);
}

static void test_fault_fixup(void){
    fix_page = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    install(SIGSEGV, on_fault_fix, 0);
    fix_page[10] = 42;
    CHECK(fixes == 1 && fix_page[10] == 42);
    signal(SIGSEGV, SIG_DFL);
    munmap(fix_page, PAGE);
}

/* ---------------- mmap pe fisier ---------------- */

static void test_file(void){
    /* Privat pe un fisier din initrd: acelasi continut ca read(); scrierea ramane in proces */
    int fd = open("/bin/hello", O_RDONLY);
    CHECK(fd >= 0);
    char head[256];
    CHECK(read(fd, head, sizeof(head)) == (ssize_t)sizeof(head));
    char *m = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(m != MAP_FAILED && memcmp(m, head, sizeof(head)) == 0 && memcmp(m, "\x7f" "ELF", 4) == 0);
    m[0] = 'X';
    CHECK(mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED && errno == EACCES);
    lseek(fd, 0, SEEK_SET);
    char c;
    CHECK(read(fd, &c, 1) == 1 && c == 0x7f);
    munmap(m, 3 * PAGE);
    close(fd);

    /* Partajat pe tmpfs: scrierile ajung in fisier (msync / munmap), se vad si in copil */
    fd = open("/tmp/mmtest.dat", O_RDWR | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    char z[2 * PAGE];
    memset(z, '.', sizeof(z));
    CHECK(write(fd, z, sizeof(z)) == (ssize_t)sizeof(z));
    char *s = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(s != MAP_FAILED && s[0] == '.');
    memcpy(s, "salut", 5);
    CHECK(msync(s, PAGE, MS_SYNC) == 0);
    char got[8] = {0};
    lseek(fd, 0, SEEK_SET);
    CHECK(read(fd, got, 5) == 5 && memcmp(got, "salut", 5) == 0);
    pid_t pid = fork();
    if(pid == 0){
        memcpy(s + PAGE, "copil", 5);   /* aceeasi pagina fizica ca la parinte (MAP_SHARED) */
        _exit(s[0] == 's' ? 0 : 1);
    }
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    CHECK(memcmp(s + PAGE, "copil", 5) == 0);
    CHECK(munmap(s, 2 * PAGE) == 0);
    lseek(fd, PAGE, SEEK_SET);
    CHECK(read(fd, got, 5) == 5 && memcmp(got, "copil", 5) == 0);
    close(fd);
    unlink("/tmp/mmtest.dat");

    /* MAP_SHARED anonim intre parinte si copil */
    int *sh = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(sh != MAP_FAILED);
    pid = fork();
    if(pid == 0){
        sh[0] = 1234;
        _exit(0);
    }
    CHECK(waitpid(pid, &st, 0) == pid && sh[0] == 1234);
    munmap(sh, PAGE);
}

/* ---------------- fork cu CoW ---------------- */

static void test_cow(void){
    size_t len = 64 * MiB;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    for(size_t i = 0; i < len; i += PAGE){
        p[i] = 'P';
    }
    int to_parent[2], to_child[2];
    CHECK(pipe(to_parent) == 0 && pipe(to_child) == 0);
    long before = mem_free_kb();
    pid_t pid = fork();
    if(pid == 0){
        long after = mem_free_kb();
        int ok = p[0] == 'P' && p[len - PAGE] == 'P';
        for(size_t i = 0; i < 4 * MiB; i += PAGE){
            p[i] = 'C';                 /* 1024 copii la scriere */
        }
        long written = mem_free_kb();
        long r[3] = {after, written, ok};
        write(to_parent[1], r, sizeof(r));
        char go;
        read(to_child[0], &go, 1);
        _exit(p[4 * MiB] == 'P' && p[0] == 'C' ? 0 : 1);   /* parintele a scris intre timp: copilul nu vede */
    }
    long r[3];
    CHECK(read(to_parent[0], r, sizeof(r)) == (ssize_t)sizeof(r));
    printf("mmtest: fork 64 MiB: %ld KiB in plus dupa fork, %ld KiB dupa 4 MiB scrisi in copil\n",
           before - r[0], before - r[1]);
    CHECK(r[2] == 1);
    CHECK(before - r[0] < 1024);        /* aproape nimic: tabele de pagini + stiva de kernel */
    CHECK(before - r[1] >= 4 * 1024 - 64 && before - r[1] < 6 * 1024);
    p[4 * MiB] = 'Q';                   /* parintele scrie dupa fork */
    CHECK(p[0] == 'P');                 /* scrierile copilului nu se vad */
    write(to_child[1], "g", 1);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
    long end = mem_free_kb();
    CHECK(munmap(p, len) == 0);
    /* Spatiul copilului se elibereaza putin dupa wait (task_destroy): pana atunci paginile comune nu revin */
    long gained = 0;
    for(int i = 0; i < 100 && (gained = mem_free_kb() - end) <= 60 * 1024; i++){
        struct timespec ts = {0, 20000000};
        nanosleep(&ts, NULL);
    }
    CHECK(gained > 60 * 1024);
    close(to_parent[0]); close(to_parent[1]); close(to_child[0]); close(to_child[1]);
}

/* ---------------- semnale ---------------- */

static volatile int usr1, usr2, chld;
static volatile sigset_t mask_in_handler;

static void on_usr1(int sig){
    (void)sig;
    usr1++;
    sigset_t m;
    sigprocmask(SIG_BLOCK, NULL, &m);
    mask_in_handler = m;
}

static void on_usr2(int sig){
    (void)sig;
    usr2++;
}

static void on_chld(int sig){
    (void)sig;
    chld++;
}

static int child_status(pid_t pid){
    int st = 0;
    pid_t r;
    do {
        r = waitpid(pid, &st, 0);
    } while(r < 0 && errno == EINTR);
    return r == pid ? st : -1;
}

/* Registrele callee-saved supravietuiesc handlerului (sigreturn reface tot frame-ul) */
static long regs_survive(void){
    register long a asm("rbx") = 0x1111;
    register long b asm("r12") = 0x2222;
    register long c asm("r15") = 0x3333;
    __asm__ volatile("" : "+r"(a), "+r"(b), "+r"(c));
    raise(SIGUSR2);
    __asm__ volatile("" : "+r"(a), "+r"(b), "+r"(c));
    return a == 0x1111 && b == 0x2222 && c == 0x3333;
}

#define STEP(n) do { if(getenv("MMDEBUG")) fprintf(stderr, "mmtest: pas %d\n", n); } while(0)

static void test_signals(void){
    set_handler(SIGUSR1, on_usr1, 0);
    set_handler(SIGUSR2, on_usr2, 0);
    CHECK(raise(SIGUSR1) == 0 && usr1 == 1);
    CHECK(sigismember((sigset_t *)&mask_in_handler, SIGUSR1) == 1);     /* blocat cat ruleaza handlerul */
    sigset_t m;
    sigprocmask(SIG_BLOCK, NULL, &m);
    CHECK(!sigismember(&m, SIGUSR1));                                  /* sigreturn a refacut masca */
    CHECK(regs_survive() && usr2 == 1);

    /* Blocat: ramane pending pana la deblocare */
    sigemptyset(&m);
    sigaddset(&m, SIGUSR2);
    sigprocmask(SIG_BLOCK, &m, NULL);
    raise(SIGUSR2);
    CHECK(usr2 == 1);
    sigset_t pend;
    CHECK(sigpending(&pend) == 0 && sigismember(&pend, SIGUSR2));
    sigprocmask(SIG_UNBLOCK, &m, NULL);
    CHECK(usr2 == 2);
    /* SIGKILL nu se poate prinde sau bloca */
    CHECK(signal(SIGKILL, SIG_IGN) == SIG_ERR && errno == EINVAL);
    /* Ignorat: nu se intampla nimic */
    signal(SIGUSR2, SIG_IGN);
    CHECK(raise(SIGUSR2) == 0 && usr2 == 2);

    /* Actiunea implicita: SIGTERM omoara, wait4 vede semnalul; SIGSEGV la acces invalid */
    pid_t pid;
    STEP(1);
    pid = fork();
    if(pid == 0){
        raise(SIGTERM);
        _exit(0);
    }
    int st = child_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    STEP(2);
    pid = fork();
    if(pid == 0){
        volatile int *bad = (volatile int *)(unsigned long)mem_free_kb();    /* adresa mica, nemapata */
        *(volatile int *)((unsigned long)bad & 0xFFF) = 1;
        _exit(0);
    }
    st = child_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV);
    /* SA_RESETHAND: al doilea semnal are actiunea implicita */
    STEP(3);
    pid = fork();
    if(pid == 0){
        set_handler(SIGUSR1, on_usr1, (int)SA_RESETHAND);
        raise(SIGUSR1);
        raise(SIGUSR1);
        _exit(0);
    }
    st = child_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGUSR1);

    /* SIGCHLD la terminarea unui copil */
    set_handler(SIGCHLD, on_chld, SA_RESTART);
    STEP(4);
    pid = fork();
    if(pid == 0){
        _exit(3);
    }
    st = child_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3 && chld == 1);
    signal(SIGCHLD, SIG_DFL);

    /* SIGPIPE: implicit omoara; ignorat -> EPIPE */
    int fds[2];
    pipe(fds);
    close(fds[0]);
    STEP(5);
    pid = fork();
    if(pid == 0){
        write(fds[1], "x", 1);
        _exit(0);
    }
    st = child_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGPIPE);
    signal(SIGPIPE, SIG_IGN);
    CHECK(write(fds[1], "x", 1) < 0 && errno == EPIPE);
    signal(SIGPIPE, SIG_DFL);
    close(fds[1]);

    /* read blocat: handler fara SA_RESTART -> EINTR; cu SA_RESTART -> read-ul continua */
    for(int restart = 0; restart < 2; restart++){
        int pc[2], ready[2];
        pipe(pc);
        pipe(ready);
        STEP(6);
    pid = fork();
        if(pid == 0){
            usr1 = 0;
            set_handler(SIGUSR1, on_usr1, restart ? SA_RESTART : 0);
            write(ready[1], "r", 1);
            char c = 0;
            ssize_t n = read(pc[0], &c, 1);
            if(restart){
                _exit(n == 1 && c == 'd' && usr1 == 1 ? 0 : 1);
            }
            _exit(n < 0 && errno == EINTR && usr1 == 1 ? 0 : 1);
        }
        char c;
        read(ready[0], &c, 1);
        struct timespec ts = {0, 50000000};
        nanosleep(&ts, NULL);           /* copilul ajunge in read */
        CHECK(kill(pid, SIGUSR1) == 0);
        nanosleep(&ts, NULL);
        write(pc[1], "d", 1);
        st = child_status(pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        close(pc[0]); close(pc[1]); close(ready[0]); close(ready[1]);
    }

    /* SIGKILL opreste un copil blocat in read; nanosleep intrerupt intoarce EINTR cu rem */
    int pc[2];
    pipe(pc);
    STEP(7);
    pid = fork();
    if(pid == 0){
        char c;
        read(pc[0], &c, 1);
        _exit(0);
    }
    struct timespec ts = {0, 50000000};
    nanosleep(&ts, NULL);
    CHECK(kill(pid, SIGKILL) == 0);
    st = child_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGKILL);
    close(pc[0]);
    close(pc[1]);
    STEP(8);
    pid = fork();
    if(pid == 0){
        set_handler(SIGUSR1, on_usr1, 0);
        struct timespec req = {5, 0}, rem = {0, 0};
        int r = nanosleep(&req, &rem);
        _exit(r < 0 && errno == EINTR && rem.tv_sec >= 3 ? 0 : 1);
    }
    nanosleep(&ts, NULL);
    kill(pid, SIGUSR1);
    st = child_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);

    /* sigsuspend: SIGUSR1 blocat, copilul il trimite, sigsuspend(gol) ruleaza handlerul si reface masca */
    usr1 = 0;
    set_handler(SIGUSR1, on_usr1, 0);
    sigemptyset(&m);
    sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, NULL);
    pid_t me = getpid();
    STEP(9);
    pid = fork();
    if(pid == 0){
        kill(me, SIGUSR1);
        _exit(0);
    }
    sigset_t empty = 0;
    CHECK(sigsuspend(&empty) < 0 && errno == EINTR && usr1 == 1);
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    CHECK(sigismember(&now, SIGUSR1));
    sigprocmask(SIG_UNBLOCK, &m, NULL);
    child_status(pid);

    /* Grupuri: kill(0) = grupul meu, kill(-pgid) = alt grup */
    STEP(10);
    pid = fork();
    if(pid == 0){
        setpgid(0, 0);
        usr2 = 0;
        set_handler(SIGUSR2, on_usr2, 0);
        kill(0, SIGUSR2);
        if(usr2 != 1 || getpgrp() != getpid()){
            _exit(1);
        }
        pause();                        /* asteapta SIGTERM de la parinte prin kill(-pgid) */
        _exit(2);
    }
    nanosleep(&ts, NULL);
    CHECK(getpgid(pid) == pid);
    CHECK(kill(-pid, SIGTERM) == 0);
    st = child_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
    CHECK(kill(-99999, SIGTERM) < 0 && errno == ESRCH);

    /* SIGFPE cu handler */
    STEP(11);
    pid = fork();
    if(pid == 0){
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = (void (*)(int))_exit;
        sigaction(SIGFPE, &sa, NULL);
        volatile int zero = 0;
        volatile int x = 10 / zero;
        (void)x;
        _exit(0);
    }
    st = child_status(pid);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == SIGFPE);   /* handlerul = _exit(sig) */
    signal(SIGUSR1, SIG_DFL);
    signal(SIGUSR2, SIG_DFL);
}

/* Ctrl-C: copilul se invarte intr-un grup propriu, in prim-plan; kernelul (test_mm) tasteaza 0x03 */
static void test_ctrl_c(void){
    pid_t pid = fork();
    if(pid == 0){
        setpgid(0, 0);
        for(volatile unsigned long i = 0; ; i++){
        }
    }
    setpgid(pid, pid);
    CHECK(tcsetpgrp(0, pid) == 0 && tcgetpgrp(0) == pid);
    int st = child_status(pid);
    CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGINT);
    CHECK(tcsetpgrp(0, getpgrp()) == 0);
}

int main(void){
    test_anon();
    test_fault_fixup();
    test_file();
    test_cow();
    test_signals();
    test_ctrl_c();
    printf("mmtest: %d verificari, %d esuate\n", checks, failed);
    if(!failed){
        printf("[OK] mmtest\n");
    }
    fflush(NULL);
    return failed ? 1 : 0;
}

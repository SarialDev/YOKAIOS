/* Starea FPU/SSE trebuie sa fie a fiecarui proces: comutari de task, fork, semnale, execve (kernel/arch/x86_64/fpu). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>
#include <sys/wait.h>
#include <time.h>

static int failures;

static void check(int ok, const char *what){
    if(!ok){
        failures++;
        printf("fputest: FAIL %s\n", what);
    }
}

static volatile double sink;
static volatile int got;
static volatile int got_int;

static void on_int(int sig){
    (void)sig;
    got_int = 1;
    signal(SIGINT, SIG_DFL);           /* ca laction din lua.c */
}

static void handler(int sig){
    (void)sig;
    // strica XMM0..XMM7: sigreturn trebuie sa le refaca
    __asm__ volatile("pxor %%xmm0, %%xmm0\n\tpxor %%xmm1, %%xmm1\n\tpcmpeqd %%xmm2, %%xmm2" ::: "xmm0", "xmm1", "xmm2");
    sink = sin(1.0) + 3.0;
    got++;
}

/* Suma 1/2^k pana la 40 e exacta in double: orice valoare stricata de o comutare se vede */
static int worker(double base, int rounds){
    double acc = 0.0;
    for(int r = 0; r < rounds; r++){
        double term = base;
        for(int k = 0; k < 40; k++){
            acc += term;
            term *= 0.5;
        }
        acc -= base * (2.0 - ldexp(1.0, -39));       // acc revine la 0 dupa fiecare runda
        if(acc != 0.0){
            return 0;
        }
        if(r % 64 == 0){
            // x87 la mijloc: fyl2x/fscale intre comutari
            if(fabs(pow(base, 2.0) - base * base) > 1e-9 * base * base){
                return 0;
            }
        }
    }
    return 1;
}

int main(int argc, char **argv){
    if(argc > 1 && strcmp(argv[1], "child") == 0){
        // dupa execve: starea implicita (MXCSR, XMM zero, x87 initializat)
        unsigned mxcsr;
        double x0;
        __asm__ volatile("stmxcsr %0\n\tmovsd %%xmm7, %1" : "=m"(mxcsr), "=m"(x0));
        unsigned short fcw;
        __asm__ volatile("fnstcw %0" : "=m"(fcw));
        check(mxcsr == 0x1F80, "MXCSR implicit dupa execve");
        check(x0 == 0.0, "XMM zero dupa execve");
        check(fcw == 0x37F, "FCW implicit dupa execve");
        return failures ? 1 : 0;
    }
    // 1. comutari: patru copii calculeaza in paralel, fiecare cu alte valori
    pid_t pids[4];
    for(int i = 0; i < 4; i++){
        pid_t p = fork();
        if(p == 0){
            _exit(worker(1.0 + i * 0.25, 3000) ? 0 : 1);
        }
        pids[i] = p;
    }
    check(worker(5.5, 3000), "parintele sub concurenta");
    for(int i = 0; i < 4; i++){
        int st = 0;
        waitpid(pids[i], &st, 0);
        check(st == 0, "copil cu calcul FPU");
    }
    // 2. fork pastreaza registrele: copilul continua cu XMM-urile parintelui
    {
        double v = 2.5;
        pid_t p = fork();
        if(p == 0){
            _exit(v == 2.5 && sqrt(v * 2.0) > 2.2 ? 0 : 1);
        }
        int st = 1;
        waitpid(p, &st, 0);
        check(st == 0, "fork pastreaza valorile double");
    }
    // 3. semnal: handlerul foloseste XMM, codul intrerupt isi primeste registrele inapoi
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handler;
        sigaction(SIGUSR1, &sa, NULL);
        double in = 123.456, out = 0;
        long pid = getpid();
        __asm__ volatile(
            "movsd %2, %%xmm0\n\t"
            "movq $62, %%rax\n\t"            // SYS_KILL
            "movq %3, %%rdi\n\t"
            "movq $10, %%rsi\n\t"            // SIGUSR1
            "syscall\n\t"
            "movsd %%xmm0, %0\n\t"
            "movq %%xmm1, %1"
            : "=m"(out), "=r"(pid) : "m"(in), "r"(pid) : "rax", "rdi", "rsi", "rcx", "r11", "xmm0", "xmm1", "memory");
        check(out == 123.456, "XMM0 refacut de sigreturn");
    }
    // 4. execve: starea FPU se reseteaza
    {
        pid_t p = fork();
        if(p == 0){
            __asm__ volatile("movq %0, %%xmm7\n\tldmxcsr %1" : : "r"(0x4008000000000000ULL), "m"((unsigned){0x1FC0}) : "xmm7");
            char *args[] = {(char *)"fputest", (char *)"child", NULL};
            execve("/bin/fputest", args, environ);
            _exit(99);
        }
        int st = 1;
        waitpid(p, &st, 0);
        check(st == 0, "starea FPU implicita dupa execve");
    }
    // 5. semnale asincrone (trimise de alt proces) intrerup codul in orice punct: registrele si XMM raman intacte
    {
        got = 0;
        pid_t parent = getpid();
        pid_t p = fork();
        if(p == 0){
            struct timespec ts = {0, 2000000};
            for(int i = 0; i < 50; i++){
                nanosleep(&ts, NULL);
                kill(parent, SIGUSR1);
            }
            _exit(0);
        }
        int bad = 0;
        long iters = 0;
        double acc = 0.0;
        unsigned long h = 1469598103934665603UL;
        while(got < 50){
            // lucru care tine multe registre vii intre intreruperi
            double a = (double)(iters & 1023) * 0.5;
            acc += a;
            h = (h ^ (unsigned long)iters) * 1099511628211UL;
            iters++;
            if(iters > 400000000L){
                break;
            }
        }
        unsigned long h2 = 1469598103934665603UL;
        double acc2 = 0.0;
        for(long i = 0; i < iters; i++){
            acc2 += (double)(i & 1023) * 0.5;
            h2 = (h2 ^ (unsigned long)i) * 1099511628211UL;
        }
        bad = !(h == h2 && acc == acc2);
        int st = 1;
        waitpid(p, &st, 0);
        check(got == 50, "toate semnalele asincrone livrate");
        check(!bad, "calcul intact intre semnale asincrone");
    }
    // 5b. rcx si r11 vii in codul intrerupt (sysret le-ar strica la intoarcerea din handler)
    {
        got = 0;
        pid_t parent = getpid();
        pid_t p = fork();
        if(p == 0){
            struct timespec ts = {0, 2000000};
            for(int i = 0; i < 30; i++){
                nanosleep(&ts, NULL);
                kill(parent, SIGUSR1);
            }
            _exit(0);
        }
        unsigned long bad = 0, spins = 0;
        while(got < 30 && spins < 400000000UL){
            unsigned long a = 0x1122334455667788UL, b = 0x99AABBCCDDEEFF00UL;
            unsigned long ra, rb;
            // a e b stau in rcx si r11; bucla scurta ca sa fie intrerupta des
            __asm__ volatile(
                "movq %2, %%rcx\n\t"
                "movq %3, %%r11\n\t"
                "movl $20000, %%eax\n\t"
                "1: decl %%eax\n\t"
                "jnz 1b\n\t"
                "movq %%rcx, %0\n\t"
                "movq %%r11, %1"
                : "=r"(ra), "=r"(rb) : "r"(a), "r"(b) : "rax", "rcx", "r11", "cc");
            if(ra != a || rb != b){
                bad++;
            }
            spins++;
        }
        int st = 0;
        waitpid(p, &st, 0);
        check(got == 30, "semnale livrate (rcx/r11)");
        check(bad == 0, "rcx si r11 pastrate peste handler");
    }
    // 6. Ctrl-C de la tastatura (tty, grup in prim-plan) cu handler, ca in REPL-ul Lua
    for(int round = 0; round < 8; round++){
        pid_t p = fork();
        if(p == 0){
            setpgid(0, 0);
            got_int = 0;
            signal(SIGINT, on_int);
            double acc = 0.0;
            unsigned long h = 1469598103934665603UL;
            long iters = 0;
            while(!got_int){
                acc += (double)(iters & 255) * 0.25;
                h = (h ^ (unsigned long)iters) * 1099511628211UL;
                iters++;
            }
            double acc2 = 0.0;
            unsigned long h2 = 1469598103934665603UL;
            for(long i = 0; i < iters; i++){
                acc2 += (double)(i & 255) * 0.25;
                h2 = (h2 ^ (unsigned long)i) * 1099511628211UL;
            }
            _exit(acc == acc2 && h == h2 ? 0 : 1);
        }
        setpgid(p, p);
        tcsetpgrp(0, p);
        int st = 1;
        waitpid(p, &st, 0);
        tcsetpgrp(0, getpgrp());
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "Ctrl-C tty cu handler");
    }
    if(failures){
        printf("fputest: %d esecuri\n", failures);
        return 1;
    }
    printf("[OK] fputest\n");
    return 0;
}

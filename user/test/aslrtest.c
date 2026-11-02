/* Test ASLR user (test=aslr). Programul e static-pie (ET_DYN). Fara argumente: isi tipareste asezarea, verifica
 * intervalele, porneste de doua ori "aslrtest -p" (exec nou = asezare noua) si cere ca stiva, heap-ul (brk), mmap si
 * codul (PIE) sa difere intre ele; un copil facut doar cu fork pastreaza asezarea parintelui. Cu -p doar tipareste.
 * Iesire 0 = totul bine. Liniile "aslrtest: stack=..." le compara tests/check_aslr.sh intre doua boot-uri */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/wait.h>

/* Valorile din kernel/exec/elf.h */
#define USER_STACK_TOP 0x00007FFFFFFFE000UL
#define STACK_RANGE ((1UL << 22) * 4096 + (8UL << 20))
#define ET_DYN_BASE 0x0000555500000000UL
#define PIE_RANGE ((1UL << 28) * 4096)
#define MMAP_BASE 0x0000400000000000UL
#define MMAP_RANGE ((1UL << 28) * 4096 + (64UL << 20))

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "aslrtest: FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); } \
} while(0)

struct layout {
    unsigned long stack, heap, mmap, code;
};

static int global_data = 42;
static void probe(struct layout *l);
/* volatile: compilatorul trebuie sa citeasca valorile puse de relocari, nu sa le stie dinainte */
int *volatile pdata = &global_data;
void (*volatile pfn)(struct layout *) = probe;

static void probe(struct layout *l){
    volatile int local = 0;
    l->stack = (unsigned long)&local;
    l->heap = (unsigned long)sbrk(0);
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    l->mmap = m == MAP_FAILED ? 0 : (unsigned long)m;
    if(m != MAP_FAILED){
        munmap(m, 4096);
    }
    l->code = (unsigned long)&probe;
}

static void print(FILE *f, const char *tag, const struct layout *l){
    fprintf(f, "%s stack=%lx heap=%lx mmap=%lx main=%lx\n", tag, l->stack, l->heap, l->mmap, l->code);
}

static int parse(const char *s, struct layout *l){
    const char *p = strstr(s, "stack=");
    if(!p){
        return -1;
    }
    return sscanf(p, "stack=%lx heap=%lx mmap=%lx main=%lx", &l->stack, &l->heap, &l->mmap, &l->code) == 4 ? 0 : -1;
}

/* Ruleaza fn intr-un copil (fork; cu exec daca do_exec) si citeste linia lui de pe un pipe */
static int child_layout(int do_exec, struct layout *l){
    int fds[2];
    if(pipe(fds) < 0){
        return -1;
    }
    pid_t pid = fork();
    if(pid < 0){
        return -1;
    }
    if(pid == 0){
        close(fds[0]);
        dup2(fds[1], 1);
        close(fds[1]);
        if(do_exec){
            char *argv[] = {"aslrtest", "-p", NULL};
            execv("/bin/aslrtest", argv);
            _exit(127);
        }
        struct layout c;
        probe(&c);
        print(stdout, "child", &c);
        fflush(stdout);
        _exit(0);
    }
    close(fds[1]);
    char buf[256];
    size_t n = 0;
    ssize_t r;
    while(n < sizeof(buf) - 1 && (r = read(fds[0], buf + n, sizeof(buf) - 1 - n)) > 0){
        n += (size_t)r;
    }
    buf[n] = 0;
    close(fds[0]);
    int st;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? parse(buf, l) : -1;
}

/* /proc/self/maps are o zona [stack] care contine adresa data */
static int maps_stack_has(unsigned long a){
    int fd = open("/proc/self/maps", O_RDONLY);
    if(fd < 0){
        return 0;
    }
    static char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if(n <= 0){
        return 0;
    }
    buf[n] = 0;
    for(char *line = buf; line && *line; ){
        char *nl = strchr(line, '\n');
        if(nl){
            *nl = 0;
        }
        unsigned long lo, hi;
        if(strstr(line, "[stack]") && sscanf(line, "%lx-%lx", &lo, &hi) == 2 && a >= lo && a < hi){
            return 1;
        }
        line = nl ? nl + 1 : NULL;
    }
    return 0;
}

int main(int argc, char **argv){
    struct layout me;
    probe(&me);
    if(argc > 1 && strcmp(argv[1], "-p") == 0){
        print(stdout, "exec", &me);
        return 0;
    }
    print(stdout, "aslrtest:", &me);

    /* intervalele din elf.h; stiva aliniata la 16 la intrare (rsp-ul initial are si offset sub pagina) */
    CHECK(me.stack < USER_STACK_TOP && me.stack > USER_STACK_TOP - STACK_RANGE);
    CHECK(me.code >= ET_DYN_BASE && me.code < ET_DYN_BASE + PIE_RANGE + (1UL << 20));
    CHECK(me.mmap >= MMAP_BASE && me.mmap < MMAP_BASE + MMAP_RANGE);
    CHECK(me.heap > me.code && me.heap < me.code + (64UL << 20));
    CHECK(maps_stack_has(me.stack));
    /* relocarile R_X86_64_RELATIVE au fost aplicate: un pointer static arata unde trebuie */
    CHECK(pdata == &global_data && *pdata == 42 && pfn == probe);
    char *h = malloc(100);
    CHECK(h && (unsigned long)h >= me.heap - 4096 && (unsigned long)h < me.heap + (1UL << 20));
    free(h);

    /* fork pastreaza asezarea */
    struct layout f;
    CHECK(child_layout(0, &f) == 0);
    CHECK(f.code == me.code && f.heap >= me.heap && f.heap < me.heap + (1UL << 20) && (f.stack >> 20) == (me.stack >> 20));

    /* doua exec-uri: tot diferit (si fata de procesul curent) */
    struct layout a, b;
    CHECK(child_layout(1, &a) == 0);
    CHECK(child_layout(1, &b) == 0);
    print(stdout, "aslrtest: exec1", &a);
    print(stdout, "aslrtest: exec2", &b);
    CHECK(a.stack != b.stack && a.stack != me.stack);
    CHECK(a.code != b.code && a.code != me.code);
    CHECK(a.mmap != b.mmap && a.mmap != me.mmap);
    CHECK(a.heap != b.heap && a.heap != me.heap);
    CHECK((a.code & 4095) == (me.code & 4095));     /* doar baza se muta, cu pagini intregi */

    printf("aslrtest: %d verificari, %d esuate\n", checks, failed);
    if(failed){
        return 1;
    }
    printf("[OK] aslrtest\n");
    return 0;
}

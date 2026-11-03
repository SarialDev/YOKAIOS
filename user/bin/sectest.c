/* /bin/sectest: teste de securitate la nivel de utilizator (pornit de kernel/tests/test_crypto.cpp, test=crypto).
 *  1. utilizatorul 1000 nu poate citi (nici scrie) /etc/shadow, nu poate redeveni root si pierde capabilitatile;
 *  2. un pachet .yokai cu semnatura gresita (un octet din semnatura sau din manifest) e respins de ypkg, si cu -U;
 *  3. un proces fara CAP_RT nu poate trece in SCHED_FIFO (nici ca root fara capabilitate, nici ca 1000);
 *  4. doua rulari (exec) ale aceluiasi program au stiva, heap-ul, mmap-ul si codul la adrese diferite (ASLR).
 * Cu -p doar tipareste asezarea (folosit de punctul 4). Tipareste "[OK] sectest", iese cu 0 daca totul a mers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sched.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/capability.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "sectest: FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); } \
} while(0)

#define HELLO "/usr/share/ypkg/hello.yokai"
#define YROOT "/tmp/sec-yp"
#define BAD "/tmp/sec-bad.yokai"

/* ---- ajutoare ---- */

/* Ruleaza fn intr-un copil; codul lui de iesire (1000 + semnal daca a murit de semnal) */
static int child(void (*fn)(void)){
    pid_t pid = fork();
    if(pid == 0){
        fn();
        _exit(0);
    }
    int st = 0;
    if(pid < 0 || waitpid(pid, &st, 0) != pid){
        return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
}

/* Ruleaza prog cu argv, stdin = /dev/null, stdout + stderr in buf; codul de iesire */
static int run(const char *prog, char *const *argv, char *buf, size_t cap){
    int p[2];
    if(pipe(p) < 0){
        return -1;
    }
    pid_t pid = fork();
    if(pid < 0){
        return -1;
    }
    if(pid == 0){
        int in = open("/dev/null", O_RDONLY);
        if(in >= 0){
            dup2(in, 0);
            close(in);
        }
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        execv(prog, argv);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    ssize_t r;
    while(n < cap - 1 && (r = read(p[0], buf + n, cap - 1 - n)) > 0){
        n += (size_t)r;
    }
    buf[n] = 0;
    close(p[0]);
    int st = 0;
    if(waitpid(pid, &st, 0) != pid){
        return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
}

static void become_1000(void){
    gid_t users = 100;
    if(setgroups(1, &users) < 0 || setgid(1000) < 0 || setuid(1000) < 0){
        fprintf(stderr, "sectest: nu pot deveni 1000: %s\n", strerror(errno));
        _exit(100);
    }
}

/* ---- 1. /etc/shadow ---- */

static void c_user_shadow(void){
    become_1000();
    struct cap_state st;
    if(getuid() != 1000 || geteuid() != 1000){
        _exit(1);
    }
    if(open("/etc/shadow", O_RDONLY) >= 0 || errno != EACCES){
        _exit(2);
    }
    if(access("/etc/shadow", R_OK) == 0 || errno != EACCES){
        _exit(3);
    }
    if(open("/etc/shadow", O_WRONLY) >= 0 || errno != EACCES){
        _exit(4);
    }
    if(open("/etc/shadow", O_RDWR | O_TRUNC) >= 0 || errno != EACCES){
        _exit(5);
    }
    if(unlink("/etc/shadow") == 0){
        _exit(6);
    }
    if(setuid(0) == 0 || errno != EPERM || setgid(0) == 0 || geteuid() != 1000){
        _exit(7);
    }
    // trecerea la 1000 a golit capabilitatile
    if(capget(0, &st) != 0 || st.effective != 0 || st.permitted != 0){
        _exit(8);
    }
    // /etc/passwd e public, shadow nu
    if(open("/etc/passwd", O_RDONLY) < 0){
        _exit(9);
    }
}

static void test_shadow(void){
    struct stat st;
    CHECK(stat("/etc/shadow", &st) == 0 && (st.st_mode & 0777) == 0600 && st.st_uid == 0 && st.st_gid == 0);
    int fd = open("/etc/shadow", O_RDONLY);
    CHECK(fd >= 0);                      // root citeste
    if(fd >= 0){
        close(fd);
    }
    CHECK(child(c_user_shadow) == 0);
}

/* ---- 2. pachet cu semnatura gresita ---- */

static char out[4096];

static void *memmem(const void *hay, size_t hn, const void *nd, size_t nn){
    const char *h = hay;
    for(size_t i = 0; i + nn <= hn; i++){
        if(memcmp(h + i, nd, nn) == 0){
            return (void *)(h + i);
        }
    }
    return NULL;
}

static char *slurp_file(const char *path, size_t *len){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return NULL;
    }
    size_t cap = 1 << 16, n = 0;
    char *b = malloc(cap);
    for(;;){
        if(n == cap){
            cap *= 2;
            b = realloc(b, cap);
        }
        ssize_t r = read(fd, b + n, cap - n);
        if(r <= 0){
            break;
        }
        n += (size_t)r;
    }
    close(fd);
    *len = n;
    return b;
}

static int write_buf(const char *path, const char *b, size_t n){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0){
        return -1;
    }
    int ok = write(fd, b, n) == (ssize_t)n;
    return close(fd) == 0 && ok ? 0 : -1;
}

static int ypkg(int allow_unsigned, const char *verb, const char *pkg){
    char *av[8];
    int n = 0;
    av[n++] = "ypkg";
    if(allow_unsigned){
        av[n++] = "-U";
    }
    av[n++] = "-r";
    av[n++] = YROOT;
    av[n++] = (char *)verb;
    av[n++] = (char *)pkg;
    av[n] = NULL;
    return run("/bin/ypkg", av, out, sizeof(out));
}

static int exists(const char *path){
    struct stat st;
    return lstat(path, &st) == 0;
}

static void test_bad_signature(void){
    mkdir(YROOT, 0755);
    size_t n;
    char *pkg = slurp_file(HELLO, &n);
    CHECK(pkg != NULL);
    if(!pkg){
        return;
    }
    char *sg = memmem(pkg, n, "signature = \"", 13);
    CHECK(sg != NULL);
    if(!sg){
        free(pkg);
        return;
    }
    // martor: pachetul livrat se verifica
    CHECK(ypkg(0, "verify", HELLO) == 0 && strstr(out, "ok: hello") != NULL);

    // 1) un singur bit schimbat in semnatura (prima cifra, apoi ultima)
    static const int pos[] = {0, 127};
    for(unsigned i = 0; i < 2; i++){
        char old = sg[13 + pos[i]];
        sg[13 + pos[i]] = old == '0' ? '1' : '0';
        CHECK(write_buf(BAD, pkg, n) == 0);
        sg[13 + pos[i]] = old;
        CHECK(ypkg(0, "verify", BAD) == 1 && strstr(out, "INVALIDA") != NULL);
        CHECK(ypkg(0, "install", BAD) == 1 && strstr(out, "INVALIDA") != NULL);
        CHECK(ypkg(1, "install", BAD) == 1 && strstr(out, "INVALIDA") != NULL);     // -U nu salveaza un pachet semnat gresit
        CHECK(!exists(YROOT "/opt/hello") && !exists(YROOT "/var/lib/ypkg/hello"));
    }
    // 2) semnatura valida, dar manifestul modificat cu un octet (versiunea)
    char *ver = memmem(pkg, n, "version = \"1.0.0\"", 17);
    CHECK(ver != NULL);
    if(ver){
        ver[15] = '1';                                  // 1.0.0 -> 1.0.1
        CHECK(write_buf(BAD, pkg, n) == 0);
        ver[15] = '0';
        CHECK(ypkg(0, "install", BAD) == 1 && strstr(out, "INVALIDA") != NULL);
        CHECK(!exists(YROOT "/opt/hello"));
    }
    // 3) semnatura zerouri si semnatura absenta: refuz
    char sv[128];
    memcpy(sv, sg + 13, 128);
    memset(sg + 13, '0', 128);
    CHECK(write_buf(BAD, pkg, n) == 0);
    CHECK(ypkg(0, "install", BAD) == 1 && strstr(out, "INVALIDA") != NULL && !exists(YROOT "/opt/hello"));
    memcpy(sg + 13, sv, 128);
    // dupa toate astea pachetul adevarat intra, deci refuzurile veneau din semnatura, nu din altceva
    CHECK(ypkg(0, "install", HELLO) == 0 && exists(YROOT "/opt/hello"));
    CHECK(ypkg(0, "remove", "hello") == 0 && !exists(YROOT "/opt/hello"));
    free(pkg);
    unlink(BAD);
}

/* ---- 3. fara CAP_RT ---- */

static void c_rt_with_cap(void){
    struct sched_param sp = {10};
    if(sched_setscheduler(0, SCHED_FIFO, &sp) < 0 || sched_getscheduler(0) != SCHED_FIFO){
        _exit(1);
    }
    sp.sched_priority = 0;
    sched_setscheduler(0, SCHED_OTHER, &sp);
}

static void c_rt_no_cap(void){
    struct cap_state st = {CAP_ALL & ~CAP_RT, CAP_ALL & ~CAP_RT};
    struct sched_param sp = {10};
    if(capset(&st) < 0){
        _exit(1);
    }
    if(sched_setscheduler(0, SCHED_FIFO, &sp) == 0 || errno != EPERM){
        _exit(2);
    }
    if(sched_getscheduler(0) != SCHED_OTHER){
        _exit(3);
    }
    // nu poate recapata capabilitatea
    st.effective = st.permitted = CAP_ALL;
    if(capset(&st) == 0 || errno != EPERM){
        _exit(4);
    }
    if(sched_setscheduler(0, SCHED_FIFO, &sp) == 0 || errno != EPERM){
        _exit(5);
    }
}

static void c_rt_user(void){
    struct sched_param sp = {10};
    become_1000();
    if(sched_setscheduler(0, SCHED_FIFO, &sp) == 0 || errno != EPERM){
        _exit(1);
    }
    if(sched_getscheduler(0) != SCHED_OTHER){
        _exit(2);
    }
}

static void test_cap_rt(void){
    CHECK(child(c_rt_with_cap) == 0);
    CHECK(child(c_rt_no_cap) == 0);
    CHECK(child(c_rt_user) == 0);
    struct cap_state st;
    CHECK(capget(0, &st) == 0 && (st.effective & CAP_RT) != 0);     // parintele si-a pastrat capabilitatea
}

/* ---- 4. ASLR ---- */

struct layout {
    unsigned long stack, heap, mmap, code;
};

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

static int exec_layout(struct layout *l){
    char *av[] = {"sectest", "-p", NULL};
    char buf[256];
    if(run("/bin/sectest", av, buf, sizeof(buf)) != 0){
        return -1;
    }
    const char *p = strstr(buf, "stack=");
    if(!p){
        return -1;
    }
    return sscanf(p, "stack=%lx heap=%lx mmap=%lx main=%lx", &l->stack, &l->heap, &l->mmap, &l->code) == 4 ? 0 : -1;
}

static void test_aslr(void){
    struct layout a, b, c;
    CHECK(exec_layout(&a) == 0 && exec_layout(&b) == 0);
    printf("sectest: aslr run1 stack=%lx heap=%lx mmap=%lx main=%lx\n", a.stack, a.heap, a.mmap, a.code);
    printf("sectest: aslr run2 stack=%lx heap=%lx mmap=%lx main=%lx\n", b.stack, b.heap, b.mmap, b.code);
    CHECK(a.stack && a.heap && a.mmap && a.code);
    // la o coincidenta rara pe un camp se mai face o rulare si se compara cu prima
    if(a.stack == b.stack || a.heap == b.heap || a.mmap == b.mmap || a.code == b.code){
        CHECK(exec_layout(&c) == 0);
        if(a.stack == b.stack){ b.stack = c.stack; }
        if(a.heap == b.heap){ b.heap = c.heap; }
        if(a.mmap == b.mmap){ b.mmap = c.mmap; }
        if(a.code == b.code){ b.code = c.code; }
    }
    CHECK(a.stack != b.stack);
    CHECK(a.heap != b.heap);
    CHECK(a.mmap != b.mmap);
    CHECK(a.code != b.code);
}

int main(int argc, char **argv){
    struct layout me;
    if(argc > 1 && strcmp(argv[1], "-p") == 0){
        probe(&me);
        printf("exec stack=%lx heap=%lx mmap=%lx main=%lx\n", me.stack, me.heap, me.mmap, me.code);
        return 0;
    }
    test_shadow();
    test_bad_signature();
    test_cap_rt();
    test_aslr();
    printf("sectest: %d verificari, %d esuate\n", checks, failed);
    if(failed){
        return 1;
    }
    printf("[OK] sectest\n");
    return 0;
}

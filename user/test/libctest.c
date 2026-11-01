/* Teste pentru libc: string, ctype, stdlib, malloc, stdio, time, setjmp, procese. Iesire 0 = totul bine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <setjmp.h>
#include <time.h>
#include <assert.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mman.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "libctest: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while(0)

static int cmp_int(const void *a, const void *b){
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static void at_exit_handler(void){
    printf("libctest: atexit ok\n");
}

static void test_string(void){
    char b[64];
    CHECK(strlen("yokai") == 5);
    strcpy(b, "abc");
    strcat(b, "def");
    CHECK(strcmp(b, "abcdef") == 0);
    CHECK(strncmp("abcd", "abxx", 2) == 0 && strncmp("abcd", "abxx", 3) < 0);
    CHECK(strchr("hello", 'l') && strchr("hello", 'l')[1] == 'l');
    CHECK(strrchr("hello", 'l')[1] == 'o');
    CHECK(strchr("hello", 'z') == NULL);
    CHECK(strstr("find the needle here", "needle") != NULL && strstr("abc", "xyz") == NULL);
    CHECK(strspn("aabbc", "ab") == 4 && strcspn("hello world", " ") == 5);
    memset(b, 'x', 10);
    CHECK(b[0] == 'x' && b[9] == 'x');
    memcpy(b, "0123456789", 10);
    memmove(b + 2, b, 8);
    CHECK(memcmp(b, "0101234567", 10) == 0);
    memmove(b, b + 2, 8);
    CHECK(memcmp(b, "01234567", 8) == 0);
    char *d = strdup("copy");
    CHECK(d && strcmp(d, "copy") == 0);
    free(d);
    char tok[] = "a,b;;c";
    char *sv;
    char *t = strtok_r(tok, ",;", &sv);
    CHECK(t && strcmp(t, "a") == 0);
    t = strtok_r(NULL, ",;", &sv);
    CHECK(t && strcmp(t, "b") == 0);
    t = strtok_r(NULL, ",;", &sv);
    CHECK(t && strcmp(t, "c") == 0 && !strtok_r(NULL, ",;", &sv));
    CHECK(strcasecmp("HeLLo", "hello") == 0);
    CHECK(strncpy(b, "ab", 5) == b && b[4] == 0);
}

static void test_ctype(void){
    CHECK(isalpha('a') && !isalpha('1') && isdigit('7') && isspace('\n') && isupper('Q') && islower('q'));
    CHECK(isxdigit('f') && !isxdigit('g') && ispunct('!') && toupper('a') == 'A' && tolower('Z') == 'z');
}

static void test_stdlib(void){
    CHECK(atoi("  -42xyz") == -42);
    char *end;
    CHECK(strtol("0x1F", &end, 16) == 31 && *end == 0);
    CHECK(strtol("077", NULL, 0) == 63);
    CHECK(strtol("z", &end, 10) == 0 && *end == 'z');
    CHECK(strtoul("4294967296", NULL, 10) == 4294967296UL);
    errno = 0;
    CHECK(strtol("99999999999999999999", NULL, 10) == LONG_MAX && errno == ERANGE);
    CHECK(strtoll("-9223372036854775808", NULL, 10) == LLONG_MIN);
    CHECK(abs(-5) == 5 && labs(-5L) == 5);

    int a[2000];
    srand(7);
    for(int i = 0; i < 2000; i++){
        a[i] = rand() % 500;
    }
    qsort(a, 2000, sizeof(int), cmp_int);
    int sorted = 1;
    for(int i = 1; i < 2000; i++){
        sorted &= a[i - 1] <= a[i];
    }
    CHECK(sorted);
    int key = a[1234];
    int *f = bsearch(&key, a, 2000, sizeof(int), cmp_int);
    CHECK(f && *f == key);
    int k2 = 100000;
    CHECK(bsearch(&k2, a, 2000, sizeof(int), cmp_int) == NULL);
    CHECK(getenv("YOKAI") && strcmp(getenv("YOKAI"), "1") == 0);
    CHECK(getenv("NU_EXISTA") == NULL);
    CHECK(setenv("LIBC_T", "val", 1) == 0 && strcmp(getenv("LIBC_T"), "val") == 0);
    CHECK(unsetenv("LIBC_T") == 0 && getenv("LIBC_T") == NULL);
}

static void test_malloc(void){
    enum { N = 400 };
    unsigned char *p[N];
    srand(11);
    for(int i = 0; i < N; i++){
        size_t n = (size_t)(rand() % 3000) + 1;
        p[i] = malloc(n);
        CHECK(p[i] && ((unsigned long)p[i] & 15) == 0);
        memset(p[i], i & 0xff, n);
        p[i][0] = (unsigned char)i;
        p[i][n - 1] = (unsigned char)i;
    }
    for(int i = 0; i < N; i += 2){
        free(p[i]);
        p[i] = NULL;
    }
    for(int i = 0; i < 100; i++){
        size_t n = (size_t)(rand() % 500) + 1;
        unsigned char *q = malloc(n);
        CHECK(q != NULL);
        memset(q, 0xEE, n);
        free(q);
    }
    int ok = 1;
    for(int i = 1; i < N; i += 2){
        ok &= p[i][0] == (unsigned char)i;
        free(p[i]);
    }
    CHECK(ok);

    unsigned char *g = malloc(10);
    memset(g, 0x5A, 10);
    for(size_t n = 20; n < 300000; n *= 3){
        g = realloc(g, n);
        CHECK(g && g[0] == 0x5A && g[9] == 0x5A);
    }
    free(g);

    unsigned char *big = malloc(3 << 20);                  // mmap
    CHECK(big != NULL);
    memset(big, 0x77, 3 << 20);
    CHECK(big[0] == 0x77 && big[(3 << 20) - 1] == 0x77);
    free(big);
    int *z = calloc(1000, sizeof(int));
    int zero = 1;
    for(int i = 0; i < 1000; i++){
        zero &= z[i] == 0;
    }
    CHECK(zero);
    free(z);
    CHECK(calloc((size_t)-1, 16) == NULL && errno == ENOMEM);
    CHECK(malloc(0) != NULL);

    void *m = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(m != MAP_FAILED);
    ((char *)m)[8191] = 1;
    CHECK(munmap(m, 8192) == 0);
    char *brk0 = sbrk(0);
    char *brk1 = sbrk(10000);
    CHECK(brk1 == brk0 && sbrk(0) == brk0 + 10000);
    brk1[9999] = 3;
    CHECK(sbrk(-10000) == brk0 + 10000);
}

static void test_printf(void){
    char b[128];
    CHECK(snprintf(b, sizeof(b), "%d|%i|%u", -7, 8, 9u) == 6 && strcmp(b, "-7|8|9") == 0);
    snprintf(b, sizeof(b), "[%5s][%-5s][%.1s]", "ab", "ab", "xyz");
    CHECK(strcmp(b, "[   ab][ab   ][x]") == 0);
    snprintf(b, sizeof(b), "[%8.3d][%+d][% d][%x][%X][%o][%#o]", 42, 5, 5, 0xbeef, 0xbeef, 8, 8);
    CHECK(strcmp(b, "[     042][+5][ 5][beef][BEEF][10][010]") == 0);
    snprintf(b, sizeof(b), "%lld %llu %hhd %hd %zu", -5000000000LL, 5000000000ULL, 300, 70000, (size_t)12);
    CHECK(strcmp(b, "-5000000000 5000000000 44 4464 12") == 0);
    snprintf(b, sizeof(b), "%*d|%-*d|%.*s|%c%%", 4, 7, 4, 7, 2, "hello", 'k');
    CHECK(strcmp(b, "   7|7   |he|k%") == 0);
    snprintf(b, sizeof(b), "%p %s", (void *)0x1234, (char *)NULL);
    CHECK(strcmp(b, "0x1234 (null)") == 0);
    CHECK(snprintf(b, 5, "abcdefgh") == 8 && strcmp(b, "abcd") == 0);
    snprintf(b, sizeof(b), "%.0d|%5.2x", 0, 3);
    CHECK(strcmp(b, "|   03") == 0);

    int x, y;
    char w[16];
    unsigned long h;
    CHECK(sscanf("12 -34 word 0xff", "%d %d %15s %lx", &x, &y, w, &h) == 4);
    CHECK(x == 12 && y == -34 && strcmp(w, "word") == 0 && h == 255);
    CHECK(sscanf("a=5", "a=%d", &x) == 1 && x == 5);
}

static void test_stdio(void){
    const char *path = "/tmp/libctest.txt";
    FILE *f = fopen(path, "w");
    CHECK(f != NULL);
    if(!f){
        return;
    }
    fprintf(f, "line one\nline %d\n", 2);
    fputs("third", f);
    fputc('\n', f);
    for(int i = 0; i < 2000; i++){
        fprintf(f, "n%d\n", i);                         // peste BUFSIZ: goliri intermediare
    }
    CHECK(fclose(f) == 0);
    f = fopen(path, "r");
    CHECK(f != NULL);
    char line[64];
    CHECK(fgets(line, sizeof(line), f) && strcmp(line, "line one\n") == 0);
    CHECK(fgets(line, sizeof(line), f) && strcmp(line, "line 2\n") == 0);
    long pos = ftell(f);
    CHECK(pos == 16);
    CHECK(fgetc(f) == 't');
    ungetc('T', f);
    CHECK(fgetc(f) == 'T');
    CHECK(fseek(f, pos, SEEK_SET) == 0 && fgets(line, sizeof(line), f) && strcmp(line, "third\n") == 0);
    int count = 0;
    while(fgets(line, sizeof(line), f)){
        count++;
    }
    CHECK(count == 2000 && feof(f));
    rewind(f);
    char big[100];
    CHECK(fread(big, 1, 9, f) == 9 && memcmp(big, "line one\n", 9) == 0);
    fclose(f);

    f = fopen(path, "a");
    fprintf(f, "appended\n");
    fclose(f);
    struct stat st;
    CHECK(stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 100);
    f = fopen(path, "r+");
    CHECK(fseek(f, 0, SEEK_END) == 0 && ftell(f) == st.st_size);
    fprintf(f, "tail\n");
    fseek(f, -5, SEEK_END);
    CHECK(fgets(line, sizeof(line), f) && strcmp(line, "tail\n") == 0);
    fclose(f);

    char *l = NULL;
    size_t cap = 0;
    f = fopen(path, "r");
    CHECK(getline(&l, &cap, f) == 9 && strcmp(l, "line one\n") == 0);
    free(l);
    fclose(f);

    errno = 0;
    CHECK(fopen("/tmp/nu/exista/deloc", "r") == NULL && errno == ENOENT);
    CHECK(remove(path) == 0 && stat(path, &st) < 0 && errno == ENOENT);
    CHECK(mkdir("/tmp/libcd", 0755) == 0);
    int fd = open("/tmp/libcd/f", O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0 && write(fd, "abc", 3) == 3 && close(fd) == 0);
    DIR *d = opendir("/tmp/libcd");
    int found = 0;
    for(struct dirent *e; d && (e = readdir(d));){
        found += strcmp(e->d_name, "f") == 0;
    }
    CHECK(d && found == 1 && closedir(d) == 0);
    CHECK(unlink("/tmp/libcd/f") == 0 && rmdir("/tmp/libcd") == 0);
    d = opendir("/bin");
    found = 0;
    for(struct dirent *e; d && (e = readdir(d));){
        found += strcmp(e->d_name, "hello") == 0;
    }
    CHECK(found == 1);
    closedir(d);
    CHECK(isatty(1) && !isatty(99));
}

static jmp_buf jb;
static int depth_reached;
static volatile int stop_at = 5;

static void deep(int n){
    depth_reached = n;
    if(n == stop_at){
        longjmp(jb, 42);
    }
    if(n < 100){
        deep(n + 1);
    }
}

static void test_setjmp_time(void){
    volatile int tries = 0;
    int r = setjmp(jb);
    if(r == 0){
        tries++;
        deep(0);
        CHECK(0);
    }
    CHECK(r == 42 && tries == 1 && depth_reached == 5);

    time_t t = 951782400L;                              // 2000-02-29 00:00:00 UTC
    struct tm tm;
    gmtime_r(&t, &tm);
    CHECK(tm.tm_year == 100 && tm.tm_mon == 1 && tm.tm_mday == 29 && tm.tm_hour == 0 && tm.tm_wday == 2 && tm.tm_yday == 59);
    CHECK(timegm(&tm) == t);
    t = 1700000000L;
    gmtime_r(&t, &tm);
    char b[64];
    CHECK(strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S %a %b %j", &tm) > 0);
    CHECK(strcmp(b, "2023-11-14 22:13:20 Tue Nov 318") == 0);
    t = -86400;
    gmtime_r(&t, &tm);
    CHECK(tm.tm_year == 69 && tm.tm_mon == 11 && tm.tm_mday == 31 && tm.tm_wday == 3);

    CHECK(time(NULL) > 1700000000L);
    struct timespec a, c, req = {0, 20 * 1000000L};
    CHECK(clock_gettime(CLOCK_MONOTONIC, &a) == 0);
    CHECK(nanosleep(&req, NULL) == 0);
    clock_gettime(CLOCK_MONOTONIC, &c);
    long ms = (c.tv_sec - a.tv_sec) * 1000 + (c.tv_nsec - a.tv_nsec) / 1000000;
    CHECK(ms >= 15 && ms < 2000);
}

static void test_process(void){
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if(pid == 0){
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        printf("child says hi");                        // fara \n: stdout e acum un pipe, deci ramane in buffer
        fflush(stdout);
        _exit(7);
    }
    close(fds[1]);
    char b[64];
    ssize_t n = read(fds[0], b, sizeof(b) - 1);
    close(fds[0]);
    b[n > 0 ? n : 0] = 0;
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    CHECK(strcmp(b, "child says hi") == 0);

    // exec dintr-un copil: hello cere exact 3 argumente
    pid = fork();
    if(pid == 0){
        char *av[] = {"hello", "x", "y", NULL};
        execv("/bin/hello", av);
        _exit(99);
    }
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    errno = 0;
    CHECK(execv("/bin/nu_exista", (char *[]){"x", NULL}) == -1 && errno == ENOENT);
    CHECK(wait(&status) == -1 && errno == ECHILD);

    int d = dup(1);
    CHECK(d >= 3 && fcntl(d, F_GETFL) >= 0);
    close(d);
    errno = 0;
    CHECK(write(77, "x", 1) == -1 && errno == EBADF);
    char cwd[64];
    CHECK(getcwd(cwd, sizeof(cwd)) != NULL && strcmp(cwd, "/") == 0);
    CHECK(chdir("/bin") == 0 && access("hello", F_OK) == 0);
    chdir("/");
}

int main(int argc, char **argv){
    (void)argc;
    (void)argv;
    atexit(at_exit_handler);
    test_string();
    test_ctype();
    test_stdlib();
    test_malloc();
    test_printf();
    test_stdio();
    test_setjmp_time();
    test_process();
    assert(checks > 100);
    if(failed){
        fprintf(stderr, "libctest: %d din %d verificari au esuat\n", failed, checks);
        return 1;
    }
    printf("[OK] libctest %d verificari\n", checks);
    return 0;
}

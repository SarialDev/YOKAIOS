/* Teste pentru utilitarele de baza (/bin/ls, cat, cp, ...): le porneste cu fork+execve si verifica iesirea si codul. Iesire 0 = totul bine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "coretest: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while(0)

#define OUTCAP 4096
static char out[OUTCAP];
static char err[OUTCAP];

static void slurp(int fd, char *buf){
    size_t len = 0;
    for(;;){
        ssize_t n = read(fd, buf + len, OUTCAP - 1 - len);
        if(n <= 0){
            break;
        }
        len += (size_t)n;
    }
    buf[len] = 0;
}

/* Ruleaza /bin/<a0> cu argumentele date (lista terminata cu NULL, cel mult 6 elemente) si stdin = fisierul `input`
 * (sau /dev/null); iesirile ajung in out/err. Intoarce codul de iesire, 1000 + semnal daca a fost omorat, -1 la eroare */
static int run(const char *input, const char *a0, ...){
    char *av[8];
    va_list ap;
    int n = 0;
    va_start(ap, a0);
    for(const char *a = a0; a && n < 6; a = va_arg(ap, const char *)){
        av[n++] = (char *)a;
    }
    va_end(ap);
    av[n] = NULL;
    int po[2], pe[2];
    if(pipe(po) < 0 || pipe(pe) < 0){
        return -1;
    }
    pid_t pid = fork();
    if(pid < 0){
        return -1;
    }
    if(pid == 0){
        int in = open(input ? input : "/dev/null", O_RDONLY);
        if(in < 0){
            _exit(126);
        }
        dup2(in, 0);
        dup2(po[1], 1);
        dup2(pe[1], 2);
        close(in); close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        char path[64];
        snprintf(path, sizeof(path), "/bin/%s", a0);
        execve(path, av, environ);
        _exit(127);
    }
    close(po[1]);
    close(pe[1]);
    slurp(po[0], out);
    slurp(pe[0], err);
    close(po[0]);
    close(pe[0]);
    int status = 0;
    if(waitpid(pid, &status, 0) != pid){
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + WTERMSIG(status);
}

static int eq(const char *a, const char *b){
    return strcmp(a, b) == 0;
}

static int has(const char *hay, const char *needle){
    return strstr(hay, needle) != NULL;
}

static void write_file(const char *path, const char *text){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd >= 0){
        write(fd, text, strlen(text));
        close(fd);
    }
}

static void test_help(void){
    static const char *const tools[] = {"ls", "cat", "echo", "mkdir", "rm", "cp", "mv", "touch", "ps", "kill", "uptime", "free",
        "date", "mount", "umount", "uname", "head", "tail", "wc", "grep", "clear", "rmdir", "env", "sleep",
        "id", "whoami", "chmod", "chown", "su"};
    for(unsigned i = 0; i < sizeof(tools) / sizeof(tools[0]); i++){
        int rc = run(NULL, tools[i], "--help", NULL, NULL, NULL);
        CHECK(rc == 0);
        // o singura linie, "uz: <nume> ..."
        size_t n = strlen(out);
        CHECK(strncmp(out, "uz: ", 4) == 0 && out[n - 1] == '\n' && strchr(out, '\n') == out + n - 1);
        CHECK(has(out, tools[i]));
        if(failed){
            fprintf(stderr, "coretest: --help la %s\n", tools[i]);
        }
    }
}

static void test_files(void){
    CHECK(run(NULL, "mkdir", "/tmp/ct", NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "mkdir", "/tmp/ct", NULL, NULL, NULL) == 1);              // exista deja
    CHECK(run(NULL, "mkdir", "-p", "/tmp/ct/a/b/c", NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "mkdir", "-p", "/tmp/ct/a/b/c", NULL, NULL, NULL) == 0);  // -p nu da eroare pentru existente
    CHECK(run(NULL, "mkdir", NULL, NULL, NULL, NULL) == 2);
    CHECK(run(NULL, "touch", "/tmp/ct/t1", "/tmp/ct/a/t2", NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "touch", "/tmp/ct/nu_exista/x", NULL, NULL, NULL) == 1);
    struct stat st;
    CHECK(stat("/tmp/ct/t1", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0);

    CHECK(run(NULL, "echo", "salut", "yokai", NULL, NULL, NULL) == 0 && eq(out, "salut yokai\n"));
    CHECK(run(NULL, "echo", "-n", "fara", NULL, NULL, NULL) == 0 && eq(out, "fara"));
    CHECK(run(NULL, "echo", NULL, NULL, NULL, NULL) == 0 && eq(out, "\n"));

    write_file("/tmp/ct/f.txt", "unu\ndoi Doi\ntrei\npatru\ncinci\nsase\n");
    CHECK(run(NULL, "cat", "/tmp/ct/f.txt", NULL, NULL, NULL) == 0 && eq(out, "unu\ndoi Doi\ntrei\npatru\ncinci\nsase\n"));
    CHECK(run("/tmp/ct/f.txt", "cat", NULL, NULL, NULL, NULL) == 0 && has(out, "trei"));
    CHECK(run(NULL, "cat", "/tmp/ct/nu_exista", NULL, NULL, NULL) == 1 && has(err, "cat: /tmp/ct/nu_exista"));

    // ls: ordonat, fara fisiere ascunse; -a le arata; -l are mod + marime
    write_file("/tmp/ct/.ascuns", "x");
    CHECK(run(NULL, "ls", "/tmp/ct", NULL, NULL, NULL) == 0 && eq(out, "a\nf.txt\nt1\n"));
    CHECK(run(NULL, "ls", "-a", "/tmp/ct", NULL, NULL, NULL) == 0 && has(out, ".ascuns\n"));
    CHECK(run(NULL, "ls", "-l", "/tmp/ct", NULL, NULL, NULL) == 0 && has(out, "drwxr-xr-x") && has(out, "-rw-r--r--") &&
          has(out, "f.txt") && has(out, "      34 f.txt"));
    CHECK(run(NULL, "ls", "-la", "/tmp/ct", NULL, NULL, NULL) == 0 && has(out, ".ascuns"));
    CHECK(run(NULL, "ls", "/tmp/ct/nu_exista", NULL, NULL, NULL) == 1);
    CHECK(run(NULL, "ls", "-z", NULL, NULL, NULL) == 2);

    // cp: fisier, in director, -r
    CHECK(run(NULL, "cp", "/tmp/ct/f.txt", "/tmp/ct/g.txt", NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "cat", "/tmp/ct/g.txt", NULL, NULL, NULL) == 0 && has(out, "patru"));
    CHECK(run(NULL, "cp", "/tmp/ct/f.txt", "/tmp/ct/a", NULL, NULL, NULL) == 0);
    CHECK(stat("/tmp/ct/a/f.txt", &st) == 0 && st.st_size == 34);
    CHECK(run(NULL, "cp", "/tmp/ct/a", "/tmp/ct/d", NULL, NULL, NULL) == 1);              // director fara -r
    CHECK(run(NULL, "cp", "-r", "/tmp/ct/a", "/tmp/ct/d", NULL) == 0);
    CHECK(stat("/tmp/ct/d/b/c", &st) == 0 && S_ISDIR(st.st_mode) && stat("/tmp/ct/d/f.txt", &st) == 0 && st.st_size == 34);
    CHECK(run(NULL, "cp", "-r", "/tmp/ct/a", "/tmp/ct/a/b/in_sine", NULL) == 1);
    CHECK(run(NULL, "cp", "/tmp/ct/nu_exista", "/tmp/ct/x", NULL, NULL, NULL) == 1);
    CHECK(run(NULL, "cp", "/tmp/ct/f.txt", NULL, NULL, NULL, NULL) == 2);

    // mv: redenumire si mutare intr-un director
    CHECK(run(NULL, "mv", "/tmp/ct/g.txt", "/tmp/ct/h.txt", NULL, NULL, NULL) == 0);
    CHECK(stat("/tmp/ct/g.txt", &st) < 0 && stat("/tmp/ct/h.txt", &st) == 0);
    CHECK(run(NULL, "mv", "/tmp/ct/h.txt", "/tmp/ct/d", NULL, NULL, NULL) == 0 && stat("/tmp/ct/d/h.txt", &st) == 0);
    CHECK(run(NULL, "mv", "/tmp/ct/nu_exista", "/tmp/ct/y", NULL, NULL, NULL) == 1);
    CHECK(run(NULL, "mv", "/tmp/ct/a", NULL, NULL, NULL, NULL) == 2);

    // rm: fisier, director fara -r, -r
    CHECK(run(NULL, "rm", "/tmp/ct/t1", NULL, NULL, NULL) == 0 && stat("/tmp/ct/t1", &st) < 0);
    CHECK(run(NULL, "rm", "/tmp/ct/t1", NULL, NULL, NULL) == 1);
    CHECK(run(NULL, "rm", "-f", "/tmp/ct/t1", NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "rm", "/tmp/ct/d", NULL, NULL, NULL) == 1);
    CHECK(run(NULL, "rm", "-r", "/tmp/ct/d", NULL, NULL, NULL) == 0 && stat("/tmp/ct/d", &st) < 0);
    CHECK(run(NULL, "rmdir", "/tmp/ct/a/b/c", NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "rmdir", "/tmp/ct/a", NULL, NULL, NULL) == 1);                       // nu e gol
    CHECK(run(NULL, "rm", "-r", "/tmp/ct/a", NULL, NULL, NULL) == 0);
}

static void test_text(void){
    const char *f = "/tmp/ct/f.txt";
    CHECK(run(NULL, "head", "-n", "2", f, NULL, NULL) == 0 && eq(out, "unu\ndoi Doi\n"));
    CHECK(run(NULL, "head", "-1", f, NULL, NULL, NULL) == 0 && eq(out, "unu\n"));
    CHECK(run(NULL, "head", f, NULL, NULL, NULL, NULL) == 0 && eq(out, "unu\ndoi Doi\ntrei\npatru\ncinci\nsase\n"));
    CHECK(run(f, "head", "-n2", NULL, NULL, NULL, NULL) == 0 && eq(out, "unu\ndoi Doi\n"));
    CHECK(run(NULL, "head", "-n", "x", f, NULL, NULL) == 2);
    CHECK(run(NULL, "head", "/tmp/ct/nu_exista", NULL, NULL, NULL, NULL) == 1);
    CHECK(run(NULL, "tail", "-n", "2", f, NULL, NULL) == 0 && eq(out, "cinci\nsase\n"));
    CHECK(run(NULL, "tail", "-n", "100", f, NULL, NULL) == 0 && eq(out, "unu\ndoi Doi\ntrei\npatru\ncinci\nsase\n"));
    CHECK(run(NULL, "tail", "-n", "0", f, NULL, NULL) == 0 && eq(out, ""));
    CHECK(run(f, "tail", "-1", NULL, NULL, NULL, NULL) == 0 && eq(out, "sase\n"));
    write_file("/tmp/ct/nonl.txt", "a\nb\nc");               // fara '\n' final
    CHECK(run(NULL, "tail", "-n", "2", "/tmp/ct/nonl.txt", NULL, NULL) == 0 && eq(out, "b\nc"));
    CHECK(run(NULL, "head", f, "/tmp/ct/nonl.txt", NULL, NULL, NULL) == 0 && has(out, "==> /tmp/ct/nonl.txt <==\na\nb\nc"));

    CHECK(run(NULL, "wc", f, NULL, NULL, NULL, NULL) == 0 && eq(out, "      6      7     34 /tmp/ct/f.txt\n"));
    CHECK(run(NULL, "wc", "-l", f, NULL, NULL, NULL) == 0 && eq(out, "      6 /tmp/ct/f.txt\n"));
    CHECK(run(NULL, "wc", "-w", f, NULL, NULL, NULL) == 0 && eq(out, "      7 /tmp/ct/f.txt\n"));
    CHECK(run(NULL, "wc", "-c", f, NULL, NULL, NULL) == 0 && eq(out, "     34 /tmp/ct/f.txt\n"));
    CHECK(run(f, "wc", "-l", NULL, NULL, NULL, NULL) == 0 && eq(out, "      6\n"));
    CHECK(run(NULL, "wc", "-l", f, "/tmp/ct/nonl.txt", NULL, NULL) == 0 && has(out, "      8 total\n"));
    CHECK(run(NULL, "wc", "/tmp/ct/nu_exista", NULL, NULL, NULL, NULL) == 1);

    CHECK(run(NULL, "grep", "oi", f, NULL, NULL, NULL) == 0 && eq(out, "doi Doi\n"));
    CHECK(run(NULL, "grep", "-i", "oi", f, NULL, NULL) == 0 && eq(out, "doi Doi\n"));
    CHECK(run(NULL, "grep", "DOI", f, NULL, NULL, NULL) == 1 && eq(out, ""));
    CHECK(run(NULL, "grep", "-i", "DOI", f, NULL, NULL) == 0 && eq(out, "doi Doi\n"));
    CHECK(run(NULL, "grep", "-n", "ci", f, NULL, NULL) == 0 && eq(out, "5:cinci\n"));
    CHECK(run(NULL, "grep", "-c", "u", f, NULL, NULL) == 0 && eq(out, "2\n"));          // unu, patru
    CHECK(run(NULL, "grep", "-v", "i", f, NULL, NULL) == 0 && eq(out, "unu\npatru\nsase\n"));
    CHECK(run(NULL, "grep", "nu_se_gaseste", f, NULL, NULL, NULL) == 1);
    CHECK(run(f, "grep", "trei", NULL, NULL, NULL, NULL) == 0 && eq(out, "trei\n"));
    CHECK(run(NULL, "grep", "a", f, "/tmp/ct/nonl.txt", NULL, NULL) == 0 && has(out, "/tmp/ct/f.txt:patru\n") && has(out, "/tmp/ct/nonl.txt:a\n"));
    CHECK(run(NULL, "grep", "x", "/tmp/ct/nu_exista", NULL, NULL, NULL) == 2);
    CHECK(run(NULL, "grep", NULL, NULL, NULL, NULL, NULL) == 2);
    CHECK(run(NULL, "grep", "-E", "x", f, NULL, NULL) == 2);                            // fara optiuni extinse
    CHECK(run(NULL, "grep", "u.u", f, NULL, NULL, NULL) == 1);                          // substring, nu regex
}

static void test_system(void){
    CHECK(run(NULL, "uname", NULL, NULL, NULL, NULL, NULL) == 0 && eq(out, "YokaiOS\n"));
    CHECK(run(NULL, "uname", "-m", NULL, NULL, NULL, NULL) == 0 && eq(out, "x86_64\n"));
    CHECK(run(NULL, "uname", "-a", NULL, NULL, NULL, NULL) == 0 && strncmp(out, "YokaiOS ", 8) == 0 && has(out, " x86_64\n"));
    CHECK(run(NULL, "uname", "-q", NULL, NULL, NULL, NULL) == 2);

    CHECK(run(NULL, "uptime", NULL, NULL, NULL, NULL, NULL) == 0 && strncmp(out, "up ", 3) == 0);
    CHECK(run(NULL, "date", NULL, NULL, NULL, NULL, NULL) == 0 && has(out, " UTC\n") && out[4] == '-' && out[7] == '-');

    CHECK(run(NULL, "free", NULL, NULL, NULL, NULL, NULL) == 0 && has(out, "total") && has(out, "Mem:") && has(out, "Swap:"));
    CHECK(run(NULL, "free", "-m", NULL, NULL, NULL, NULL) == 0 && has(out, "MiB"));
    CHECK(run(NULL, "free", "-x", NULL, NULL, NULL, NULL) == 2);

    // ps: antetul si cel putin parintele (coretest) si el insusi
    CHECK(run(NULL, "ps", NULL, NULL, NULL, NULL, NULL) == 0 && has(out, "PID") && has(out, "COMMAND") && has(out, "coretest") && has(out, " ps\n"));

    CHECK(run(NULL, "clear", NULL, NULL, NULL, NULL, NULL) == 0 && eq(out, "\033[2J\033[H"));
    CHECK(run(NULL, "env", NULL, NULL, NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "sleep", "0", NULL, NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "sleep", NULL, NULL, NULL, NULL, NULL) == 2);
}

static void test_mount(void){
    CHECK(run(NULL, "mount", NULL, NULL, NULL, NULL, NULL) == 0 && has(out, " on /proc type proc ") && has(out, " on /tmp type tmpfs "));
    CHECK(run(NULL, "mkdir", "/tmp/ct/mnt", NULL, NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "mount", "-t", "tmpfs", "none", "/tmp/ct/mnt", NULL) == 0);
    CHECK(run(NULL, "mount", NULL, NULL, NULL, NULL, NULL) == 0 && has(out, "none on /tmp/ct/mnt type tmpfs (rw)"));
    CHECK(run(NULL, "touch", "/tmp/ct/mnt/in_montare", NULL, NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "ls", "/tmp/ct/mnt", NULL, NULL, NULL, NULL) == 0 && eq(out, "in_montare\n"));
    CHECK(run(NULL, "umount", "/tmp/ct/mnt", NULL, NULL, NULL, NULL) == 0);
    CHECK(run(NULL, "ls", "/tmp/ct/mnt", NULL, NULL, NULL, NULL) == 0 && eq(out, ""));    // directorul de dedesubt e gol: fisierul era in tmpfs-ul montat
    CHECK(run(NULL, "mount", NULL, NULL, NULL, NULL, NULL) == 0 && !has(out, "/tmp/ct/mnt"));
    CHECK(run(NULL, "umount", "/tmp/ct/mnt", NULL, NULL, NULL, NULL) == 1);               // nu mai e montat
    CHECK(run(NULL, "mount", "-t", "tmpfs", "-o", "ro", "none", NULL) == 2);                    // lipseste destinatia
    CHECK(run(NULL, "mount", "none", "/tmp/ct/mnt", NULL, NULL, NULL) == 2);             // lipseste -t
    CHECK(run(NULL, "mount", "-t", "nu_exista", "none", "/tmp/ct/mnt", NULL) == 1);
    CHECK(run(NULL, "umount", NULL, NULL, NULL, NULL, NULL) == 2);
}

/* kill: un copil care doarme si unul care se invarte in user mode; ambele mor, cu semnalul in status */
static void test_kill(void){
    pid_t sleeper = fork();
    if(sleeper == 0){
        static const char *const av[] = {"sleep", "100", NULL};
        execve("/bin/sleep", (char *const *)av, environ);
        _exit(127);
    }
    pid_t spinner = fork();
    if(spinner == 0){
        for(;;){
            __asm__ volatile("");
        }
    }
    CHECK(sleeper > 0 && spinner > 0);
    sched_yield();
    char pid1[16], pid2[16];
    snprintf(pid1, sizeof(pid1), "%d", sleeper);
    snprintf(pid2, sizeof(pid2), "%d", spinner);
    CHECK(run(NULL, "kill", "-0", pid1, pid2, NULL, NULL) == 0);                          // exista
    CHECK(run(NULL, "kill", pid1, NULL, NULL, NULL, NULL) == 0);                          // TERM implicit
    CHECK(run(NULL, "kill", "-KILL", pid2, NULL, NULL, NULL) == 0);
    int status = 0;
    CHECK(waitpid(sleeper, &status, 0) == sleeper && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    CHECK(waitpid(spinner, &status, 0) == spinner && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(run(NULL, "kill", pid1, NULL, NULL, NULL, NULL) == 1 && has(err, "kill:"));     // a disparut
    CHECK(run(NULL, "kill", "-9", "abc", NULL, NULL, NULL) == 1);
    CHECK(run(NULL, "kill", "-NUMEFALS", "1", NULL, NULL, NULL) == 2);
    CHECK(run(NULL, "kill", NULL, NULL, NULL, NULL, NULL) == 2);
    CHECK(kill(-1, 9) < 0 && errno == EINVAL);                                         // "toate procesele" nu exista
    CHECK(kill(99999, 0) < 0 && errno == ESRCH);
    CHECK(kill(getpid(), 0) == 0);
}

int main(void){
    test_help();
    test_files();
    test_text();
    test_system();
    test_mount();
    test_kill();
    // curata
    run(NULL, "rm", "-r", "/tmp/ct", NULL, NULL, NULL);
    printf("coretest: %d verificari, %d esuate\n", checks, failed);
    if(failed){
        return 1;
    }
    printf("[OK] coretest\n");
    return 0;
}

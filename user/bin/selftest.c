/* /bin/selftest: testul de ansamblu al userspace-ului, pornit de `init -t` (test=userspace).
 * Ruleaza utilitarele si compara iesirile, pipe-uri si redirectari prin `ysh -c`, scriptul /etc/selftest.lua,
 * un fork cu copy-on-write, instalarea pachetului hello.yokai cu ypkg si utilizatorii (id, su, drepturi). Tipareste "[OK] userspace" si iese cu 0
 * daca totul a mers; altfel cate o linie "selftest: FAIL ..." pentru fiecare verificare picata si iese cu 1 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "selftest: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while(0)

#define OUTCAP 8192
static char out[OUTCAP];    /* stdout + stderr ale ultimei comenzi */

#define DIR "/tmp/selftest"

/* Ruleaza programul (cale absoluta) cu argumentele date (lista terminata cu NULL, cel mult 8) si stdin = /dev/null;
 * stdout si stderr ajung amandoua in out[]. Intoarce codul de iesire, 1000 + semnal daca a fost omorat, -1 la eroare */
static int run(const char *path, ...){
    char *av[10];
    va_list ap;
    int n = 0;
    va_start(ap, path);
    for(const char *a = va_arg(ap, const char *); a && n < 8; a = va_arg(ap, const char *)){
        av[n++] = (char *)a;
    }
    va_end(ap);
    av[n] = NULL;
    int po[2];
    if(pipe(po) < 0){
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
        dup2(po[1], 1);
        dup2(po[1], 2);
        close(po[0]);
        close(po[1]);
        execve(path, av, environ);
        _exit(127);
    }
    close(po[1]);
    size_t len = 0;
    for(;;){
        ssize_t r = read(po[0], out + len, OUTCAP - 1 - len);
        if(r <= 0){
            break;
        }
        len += (size_t)r;
    }
    out[len] = 0;
    close(po[0]);
    int status = 0;
    if(waitpid(pid, &status, 0) != pid){
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + WTERMSIG(status);
}

/* `ysh -c "<linie>"`: codul de iesire, iesirea in out[] */
static int sh(const char *line){
    return run("/bin/ysh", "ysh", "-c", line, NULL);
}

static int has(const char *s){
    return strstr(out, s) != NULL;
}

static int same(const char *s){
    if(strcmp(out, s) != 0){
        fprintf(stderr, "selftest: astept \"%s\", am primit \"%s\"\n", s, out);
        return 0;
    }
    return 1;
}

static void write_file(const char *path, const char *text){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if(fd >= 0){
        CHECK(write(fd, text, strlen(text)) == (ssize_t)strlen(text));
        close(fd);
    }
}

/* ---------------- utilitare ---------------- */

static void test_tools(void){
    CHECK(run("/bin/echo", "echo", "salut", "lume", NULL) == 0 && same("salut lume\n"));
    CHECK(run("/bin/echo", "echo", "-n", "fara", NULL) == 0 && same("fara"));
    CHECK(run("/bin/cat", "cat", "/etc/hostname", NULL) == 0 && same("yokai-drone\n"));
    CHECK(run("/bin/uname", "uname", "-s", NULL) == 0 && same("YokaiOS\n"));
    CHECK(run("/bin/ls", "ls", "/etc", NULL) == 0 && has("hostname") && has("os-release") && has("selftest.lua"));
    CHECK(run("/bin/grep", "grep", "ID=yokai", "/etc/os-release", NULL) == 0 && same("ID=yokai\n"));
    CHECK(run("/bin/grep", "grep", "nu-exista", "/etc/os-release", NULL) == 1 && same(""));
    CHECK(run("/bin/head", "head", "-n", "1", "/etc/os-release", NULL) == 0 && same("NAME=\"YokaiOS\"\n"));
    CHECK(run("/bin/tail", "tail", "-n", "1", "/etc/os-release", NULL) == 0 && same("PRETTY_NAME=\"YokaiOS 0.1\"\n"));
    CHECK(run("/bin/wc", "wc", "-l", "/etc/os-release", NULL) == 0 && same("      4 /etc/os-release\n"));

    CHECK(run("/bin/mkdir", "mkdir", "-p", DIR "/a/b", NULL) == 0 && same(""));
    write_file(DIR "/a/f.txt", "unu\ndoi\ntrei\n");
    CHECK(run("/bin/cp", "cp", DIR "/a/f.txt", DIR "/a/b/g.txt", NULL) == 0);
    CHECK(run("/bin/cat", "cat", DIR "/a/b/g.txt", NULL) == 0 && same("unu\ndoi\ntrei\n"));
    CHECK(run("/bin/mv", "mv", DIR "/a/b/g.txt", DIR "/a/h.txt", NULL) == 0);
    CHECK(run("/bin/ls", "ls", DIR "/a", NULL) == 0 && has("h.txt") && has("f.txt"));
    CHECK(run("/bin/ls", "ls", DIR "/a/b", NULL) == 0 && same(""));
    CHECK(run("/bin/touch", "touch", DIR "/gol", NULL) == 0);
    struct stat st;
    CHECK(stat(DIR "/gol", &st) == 0 && st.st_size == 0);
    CHECK(run("/bin/cat", "cat", DIR "/nu-exista", NULL) == 1 && has("nu-exista"));
    CHECK(run("/bin/ps", "ps", NULL) == 0 && has("selftest") && has("init"));
    CHECK(run("/bin/env", "env", NULL) == 0 && has("PATH=/bin:/sbin"));
    CHECK(run("/bin/rm", "rm", "-rf", DIR "/a", NULL) == 0);
    CHECK(stat(DIR "/a", &st) < 0 && errno == ENOENT);
    CHECK(run("/bin/ls", "ls", "--help", NULL) == 0 && has("uz: "));
}

/* ---------------- ysh -c: pipe-uri, redirectari, operatori ---------------- */

static void test_shell(void){
    CHECK(sh("echo unu doi trei | wc -w") == 0 && same("      3\n"));
    CHECK(sh("cat /etc/os-release | grep VERSION | head -n 1") == 0 && same("VERSION_ID=0.1\n"));
    CHECK(sh("echo prima > " DIR "/r.txt; echo a-doua >> " DIR "/r.txt") == 0);
    CHECK(run("/bin/cat", "cat", DIR "/r.txt", NULL) == 0 && same("prima\na-doua\n"));
    CHECK(sh("wc -l < " DIR "/r.txt") == 0 && same("      2\n"));
    CHECK(sh("grep doua < " DIR "/r.txt > " DIR "/s.txt; cat " DIR "/s.txt") == 0 && same("a-doua\n"));
    CHECK(sh("cat /nu-exista || echo altfel") == 0 && has("altfel"));
    CHECK(sh("echo da && echo si asta") == 0 && same("da\nsi asta\n"));
    CHECK(sh("grep nimic /etc/hostname && echo nu") == 1 && same(""));
    CHECK(sh("exit 7") == 7);
    CHECK(sh("X=yokai; echo $X-$?") == 0 && same("yokai-0\n"));
    CHECK(sh("comanda-inexistenta") == 127);
    CHECK(sh("cd " DIR "; pwd") == 0 && same(DIR "\n"));
}

/* ---------------- Lua ---------------- */

static void test_lua(void){
    int rc = run("/bin/lua", "lua", "/etc/selftest.lua", NULL);
    CHECK(rc == 0 && has("selftest.lua: ok\n"));
    if(rc != 0){
        fprintf(stderr, "selftest: lua a iesit cu %d:\n%s", rc, out);
    }
    CHECK(run("/bin/lua", "lua", "-e", "print(6 * 7, ('x'):rep(3))", NULL) == 0 && same("42\txxx\n"));
}

/* ---------------- fork cu copy-on-write ---------------- */

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

static void test_cow(void){
    const size_t len = 16 * 1024 * 1024, page = 4096, touched = 2 * 1024 * 1024;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if(p == MAP_FAILED){
        return;
    }
    for(size_t i = 0; i < len; i += page){
        p[i] = 'P';
    }
    int up[2], down[2];
    CHECK(pipe(up) == 0 && pipe(down) == 0);
    long before = mem_free_kb();
    pid_t pid = fork();
    if(pid == 0){
        long after = mem_free_kb();
        int ok = p[0] == 'P' && p[len - page] == 'P';
        for(size_t i = 0; i < touched; i += page){
            p[i] = 'C';
        }
        long r[3] = {after, mem_free_kb(), ok};
        write(up[1], r, sizeof(r));
        char go;
        read(down[0], &go, 1);
        /* parintele a scris in p[len - page] intre timp: copilul vede tot 'P' */
        _exit(p[len - page] == 'P' && p[0] == 'C' ? 0 : 1);
    }
    CHECK(pid > 0);
    long r[3] = {0, 0, 0};
    CHECK(read(up[0], r, sizeof(r)) == (ssize_t)sizeof(r));
    CHECK(r[2] == 1);
    CHECK(before - r[0] < 1024);                                     /* fork: fara copierea celor 16 MiB */
    CHECK(before - r[1] >= (long)(touched / 1024) - 64);             /* scrierea in copil copiaza paginile */
    CHECK(before - r[1] < (long)(touched / 1024) + 1024);
    p[len - page] = 'Q';
    CHECK(p[0] == 'P');                                               /* scrierile copilului nu se vad aici */
    write(down[1], "g", 1);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(munmap(p, len) == 0);
    close(up[0]); close(up[1]); close(down[0]); close(down[1]);
}

/* ---------------- pachet .yokai ---------------- */

static void test_package(void){
    CHECK(run("/bin/ypkg", "ypkg", "verify", "/usr/share/ypkg/hello.yokai", NULL) == 0 && has("ok: hello 1.0.0"));
    CHECK(run("/bin/ypkg", "ypkg", "install", "/usr/share/ypkg/hello.yokai", NULL) == 0 && same("instalat hello 1.0.0 (2 fisiere)\n"));
    CHECK(run("/bin/ypkg", "ypkg", "list", NULL) == 0 && has("hello") && has("1.0.0"));
    CHECK(run("/opt/hello/bin/hello", "hello", "x", NULL) == 0 && has("salut de la hello 1.0.0") && has("arg 1 = \"x\""));
    CHECK(sh("ypkg run hello din-ysh | grep din-ysh") == 0 && same("  arg 1 = \"din-ysh\"\n"));
    // permisiunea cap.net din manifest = plafonul de capabilitati al executabilului (politica /var/lib/ypkg/exec.caps)
    CHECK(run("/bin/cat", "cat", "/var/lib/ypkg/exec.caps", NULL) == 0 && same("/opt/hello/bin/hello net hello\n"));
    CHECK(run("/opt/hello/bin/hello", "hello", "--caps", NULL) == 0 && same("caps: permitted=2 effective=2\n"));
    CHECK(run("/bin/caps", "caps", NULL) == 0 && has("effective: sys_admin,net,rawio,rt,mount,kill,setuid"));
    CHECK(run("/bin/ypkg", "ypkg", "install", "/usr/share/ypkg/hello.yokai", NULL) != 0 && has("deja instalat"));
    CHECK(run("/bin/ypkg", "ypkg", "remove", "hello", NULL) == 0 && same("scos hello (2 fisiere)\n"));
    struct stat st;
    CHECK(stat("/opt/hello/bin/hello", &st) < 0 && stat("/var/lib/ypkg/exec.caps", &st) < 0);
    CHECK(run("/bin/ypkg", "ypkg", "list", NULL) == 0 && !has("hello"));
}

/* ---------------- utilizatori: id, su, drepturi pe /etc si HOME (init a facut /home pe tmpfs) ---------------- */

static void test_users(void){
    CHECK(run("/bin/id", "id", NULL) == 0 && same("uid=0(root) gid=0(root) groups=0(root)\n"));
    CHECK(run("/bin/whoami", "whoami", NULL) == 0 && same("root\n"));
    CHECK(run("/bin/su", "su", "yokai", "-c", "id", NULL) == 0 &&
          same("uid=1000(yokai) gid=1000(yokai) groups=1000(yokai),100(users)\n"));
    CHECK(run("/bin/su", "su", "yokai", "-c", "touch /etc/x", NULL) == 1 && has("Permission denied"));
    CHECK(run("/bin/su", "su", "yokai", "-c", "cat /etc/shadow", NULL) == 1 && has("Permission denied"));
    CHECK(run("/bin/su", "su", "-", "yokai", "-c", "pwd; echo f > f.txt; ls -l f.txt", NULL) == 0 &&
          has("/home/yokai\n") && has("-rw-r--r-- yokai    yokai           2 f.txt\n"));
    CHECK(run("/bin/su", "su", "yokai", "-c", "ls /root", NULL) == 1 && has("Permission denied"));
    CHECK(run("/bin/ls", "ls", "-l", "/home", NULL) == 0 && has("drwx------ yokai    yokai"));
    CHECK(run("/bin/rm", "rm", "/home/yokai/f.txt", NULL) == 0);
}

/* ---------------- login, passwd, PIN, fisier-cheie (criptografie: lib/crypto) ---------------- */

#define SHADOW DIR "/shadow"
#define AUTH DIR "/auth"
#define UID_OUT "uid=1000(yokai) gid=1000(yokai) groups=1000(yokai),100(users)\n"

/* ysh -c "<comanda> < intrare": stdin vine dintr-un fisier cu liniile date */
static int sh_in(const char *input, const char *cmd){
    char line[400];
    write_file(DIR "/in", input);
    snprintf(line, sizeof(line), "%s < " DIR "/in", cmd);
    return sh(line);
}

static void test_login(void){
    write_file(SHADOW, "root:*:20454:0:99999:7:::\nyokai:!:20454:0:99999:7:::\n");
    unlink(AUTH);

    /* parola: passwd scrie hash-ul, login reuseste doar cu parola corecta */
    CHECK(sh_in("taina-yokai\ntaina-yokai\n", "passwd -s " SHADOW " -a " AUTH " yokai") == 0 && has("parola schimbata pentru yokai"));
    CHECK(sh("grep yokai: " SHADOW) == 0 && has("yokai:$pbkdf2-sha256$100000$") && !has("taina-yokai"));
    CHECK(sh("grep root: " SHADOW) == 0 && has("root:*:20454:0:99999:7:::"));
    CHECK(sh_in("taina-yokai\n", "login -s " SHADOW " -a " AUTH " -c id yokai") == 0 && has(UID_OUT));
    CHECK(sh_in("taina-yokai\n", "login -s " SHADOW " -a " AUTH " -c pwd yokai") == 0 && has("/home/yokai\n"));
    CHECK(sh_in("gresita\n", "login -s " SHADOW " -a " AUTH " -c id yokai") == 1 && has("autentificare esuata") && !has("uid="));
    CHECK(sh_in("taina-yoka\n", "login -s " SHADOW " -a " AUTH " -c id yokai") == 1 && !has("uid="));
    CHECK(sh_in("\n", "login -s " SHADOW " -a " AUTH " -c id yokai") == 1 && !has("uid="));
    CHECK(sh_in("taina-yokai\n", "login -s " SHADOW " -a " AUTH " -c id nimeni") == 1 && !has("uid="));
    CHECK(sh_in("orice\n", "login -s " SHADOW " -a " AUTH " -c id root") == 1 && !has("uid="));   /* "*" = blocat */
    CHECK(sh_in("taina-yokai\n", "login -s " SHADOW " -a " AUTH " -c id") == 1);                    /* fara nume: citeste numele din intrare */
    CHECK(sh_in("yokai\ntaina-yokai\n", "login -s " SHADOW " -a " AUTH " -c id") == 0 && has(UID_OUT));
    CHECK(sh_in("taina-yokai\n", "login -s " SHADOW " -a " AUTH " -c id ../x:y") == 1 && has("invalid"));
    /* schimbarea parolei pastreaza celelalte linii si inlocuieste hash-ul */
    CHECK(sh_in("alta-parola\nalta-parola\n", "passwd -s " SHADOW " -a " AUTH " yokai") == 0);
    CHECK(sh_in("alta-parola\n", "login -s " SHADOW " -a " AUTH " -c id yokai") == 0 && has(UID_OUT));
    CHECK(sh_in("taina-yokai\n", "login -s " SHADOW " -a " AUTH " -c id yokai") == 1);
    CHECK(sh("grep yokai: " SHADOW " | wc -l") == 0 && same("      1\n"));
    /* parole respinse de passwd */
    CHECK(sh_in("scurt\nscurt\n", "passwd -s " SHADOW " -a " AUTH " yokai") == 1 && has("6 caractere"));
    CHECK(sh_in("parola-lunga\nalta-lunga\n", "passwd -s " SHADOW " -a " AUTH " yokai") == 1 && has("nu coincid"));
    CHECK(sh_in("parola-lunga\nparola-lunga\n", "passwd -s " SHADOW " -a " AUTH " nimeni") == 1 && has("necunoscut"));

    /* PIN */
    CHECK(sh_in("4821\n4821\n", "passwd -p -s " SHADOW " -a " AUTH " yokai") == 0 && has("PIN schimbat"));
    CHECK(sh("grep yokai:pin: " AUTH) == 0 && has("yokai:pin:$pbkdf2-sha256$100000$") && !has("4821"));
    CHECK(sh_in("4821\n", "login -p -s " SHADOW " -a " AUTH " -c id yokai") == 0 && has(UID_OUT));
    CHECK(sh_in("4822\n", "login -p -s " SHADOW " -a " AUTH " -c id yokai") == 1 && !has("uid="));
    CHECK(sh_in("alta-parola\n", "login -p -s " SHADOW " -a " AUTH " -c id yokai") == 1 && !has("uid="));   /* parola nu e PIN */
    CHECK(sh_in("4821\n", "login -s " SHADOW " -a " AUTH " -c id yokai") == 1 && !has("uid="));              /* PIN-ul nu e parola */
    CHECK(sh_in("12\n12\n", "passwd -p -s " SHADOW " -a " AUTH " yokai") == 1 && has("4-16 cifre"));
    CHECK(sh_in("12ab\n12ab\n", "passwd -p -s " SHADOW " -a " AUTH " yokai") == 1 && has("4-16 cifre"));

    /* fisier-cheie sub un prefix (in productie /media/...; aici -M pentru ca /media e read-only) */
    write_file(DIR "/key.bin", "continut-secret-al-cheii\n");
    CHECK(sh("passwd -k /etc/hostname -s " SHADOW " -a " AUTH " yokai") == 1 && has("trebuie sa fie sub /media/"));
    CHECK(sh("passwd -M " DIR "/ -k " DIR "/../etc/hostname -s " SHADOW " -a " AUTH " yokai") == 1);
    CHECK(sh("passwd -M " DIR "/ -k " DIR "/key.bin -s " SHADOW " -a " AUTH " yokai") == 0 && has("fisier-cheie inrolat"));
    CHECK(sh("grep yokai:key: " AUTH) == 0 && has("yokai:key:" DIR "/key.bin:$pbkdf2-sha256$") && !has("continut-secret"));
    CHECK(sh("login -M " DIR "/ -k -s " SHADOW " -a " AUTH " -c id yokai") == 0 && has(UID_OUT));
    CHECK(sh("login -M " DIR "/ -K " DIR "/key.bin -s " SHADOW " -a " AUTH " -c id yokai") == 0 && has(UID_OUT));
    CHECK(sh("login -M /nu/ -k -s " SHADOW " -a " AUTH " -c id yokai") == 1 && !has("uid="));              /* in afara prefixului */
    write_file(DIR "/alta.bin", "alt-continut\n");
    CHECK(sh("login -M " DIR "/ -K " DIR "/alta.bin -s " SHADOW " -a " AUTH " -c id yokai") == 1 && !has("uid="));
    write_file(DIR "/key.bin", "continut-secret-al-cheii!\n");                                            /* cheie modificata */
    CHECK(sh("login -M " DIR "/ -k -s " SHADOW " -a " AUTH " -c id yokai") == 1 && !has("uid="));
    CHECK(sh("login -M " DIR "/ -k -s " SHADOW " -a " AUTH " -c id root") == 1);                           /* root nu are cheie */

    /* fisierele nu se schimba doar prin citire; ceilalti utilizatori nu pot rula login / folosi optiunile de test */
    CHECK(run("/bin/su", "su", "yokai", "-c", "login -c id yokai", NULL) == 1 && has("trebuie pornit ca root"));
    CHECK(run("/bin/su", "su", "yokai", "-c", "passwd -s " SHADOW " yokai", NULL) == 1 && has("doar root"));
    CHECK(run("/bin/su", "su", "yokai", "-c", "cat /etc/shadow", NULL) == 1 && has("Permission denied"));
    /* radacina e read-only: passwd pe /etc/shadow esueaza cu eroare clara, fisierul ramane neschimbat */
    CHECK(sh_in("parola-lunga\nparola-lunga\n", "passwd yokai") == 1 && has("passwd: /etc/shadow:"));
    CHECK(run("/bin/login", "login", "--help", NULL) == 0 && has("uz: login"));
    CHECK(run("/bin/passwd", "passwd", "--help", NULL) == 0 && has("uz: passwd"));
}

int main(void){
    mkdir(DIR, 0755);
    struct { const char *name; void (*fn)(void); } parts[] = {
        {"utilitare", test_tools}, {"ysh -c", test_shell}, {"lua", test_lua},
        {"fork cow", test_cow}, {"pachet", test_package}, {"utilizatori", test_users},
        {"login", test_login},
    };
    for(size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++){
        int before = failed;
        parts[i].fn();
        printf("selftest: %s %s\n", parts[i].name, failed == before ? "ok" : "FAIL");
    }
    run("/bin/rm", "rm", "-rf", DIR, NULL);
    if(failed){
        printf("selftest: %d din %d verificari au esuat\n", failed, checks);
        return 1;
    }
    printf("[OK] userspace (%d verificari)\n", checks);
    return 0;
}

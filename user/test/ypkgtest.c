/* Teste pentru ypkg (pachete .yokai): hello.yokai facut pe gazda de tools/mkyokai se instaleaza intr-o radacina de proba
 * (/tmp/yp), ruleaza, se listeaza, se scoate; plus pachete stricate/ostile facute aici. Iesire 0 = totul bine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "ypkgtest: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
} while(0)

#define OUTCAP 8192
static char out[OUTCAP];
static char err[OUTCAP];
#define ROOT "/tmp/yp"
#define HELLO "/usr/share/ypkg/hello.yokai"
#define EXTRA "/usr/share/ypkg/hello-extra.yokai"

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

/* Porneste prog cu argv (terminat cu NULL); iesirile in out/err; intoarce codul de iesire sau 1000 + semnal */
static int runv(const char *prog, char *const *argv){
    int po[2], pe[2];
    if(pipe(po) < 0 || pipe(pe) < 0){
        return -1;
    }
    pid_t pid = fork();
    if(pid < 0){
        return -1;
    }
    if(pid == 0){
        int in = open("/dev/null", O_RDONLY);
        dup2(in, 0);
        dup2(po[1], 1);
        dup2(pe[1], 2);
        close(in); close(po[0]); close(po[1]); close(pe[0]); close(pe[1]);
        execve(prog, argv, environ);
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

/* ypkg -r /tmp/yp <args...> */
static int ypkg(const char *a1, const char *a2, const char *a3){
    char *av[8];
    int n = 0;
    av[n++] = "ypkg";
    av[n++] = "-r";
    av[n++] = ROOT;
    if(a1){ av[n++] = (char *)a1; }
    if(a2){ av[n++] = (char *)a2; }
    if(a3){ av[n++] = (char *)a3; }
    av[n] = NULL;
    return runv("/bin/ypkg", av);
}

static int has(const char *hay, const char *needle){
    return strstr(hay, needle) != NULL;
}

static int exists(const char *path){
    struct stat st;
    return lstat(path, &st) == 0;
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

static void *memmem(const void *hay, size_t hn, const void *nd, size_t nn){
    const char *h = hay;
    for(size_t i = 0; i + nn <= hn; i++){
        if(memcmp(h + i, nd, nn) == 0){
            return (void *)(h + i);
        }
    }
    return NULL;
}

static int file_is(const char *path, const char *text){
    size_t n;
    char *b = slurp_file(path, &n);
    int ok = b && n == strlen(text) && memcmp(b, text, n) == 0;
    free(b);
    return ok;
}

static int rm_root(void){
    char *av[] = {"rm", "-rf", ROOT, NULL};
    return runv("/bin/rm", av);
}

/* ---- pachete facute de mana ---- */

static void tar_hdr(char *h, const char *name, char type, size_t size){
    memset(h, 0, 512);
    snprintf(h, 100, "%s", name);
    snprintf(h + 100, 8, "%07o", type == '5' ? 0755 : 0644);
    snprintf(h + 108, 8, "%07o", 0);
    snprintf(h + 116, 8, "%07o", 0);
    snprintf(h + 124, 12, "%011lo", (unsigned long)size);
    snprintf(h + 136, 12, "%011o", 0);
    memset(h + 148, ' ', 8);
    h[156] = type;
    memcpy(h + 257, "ustar", 6);
    memcpy(h + 263, "00", 2);
    unsigned sum = 0;
    for(int i = 0; i < 512; i++){
        sum += (unsigned char)h[i];
    }
    snprintf(h + 148, 8, "%06o", sum);
    h[155] = ' ';
}

typedef struct { const char *name; char type; const char *data; } TEnt;

static int mkpkg(const char *path, const char *manifest, const TEnt *ents, int n){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0){
        return -1;
    }
    char h[512], z[512];
    memset(z, 0, sizeof(z));
    for(int i = -1; i < n; i++){
        const char *name = i < 0 ? "manifest.toml" : ents[i].name;
        char type = i < 0 ? '0' : ents[i].type;
        const char *data = i < 0 ? manifest : ents[i].data;
        size_t len = data ? strlen(data) : 0;
        tar_hdr(h, name, type, len);
        write(fd, h, 512);
        if(len){
            write(fd, data, len);
            write(fd, z, (512 - len % 512) % 512);
        }
    }
    write(fd, z, 512);
    write(fd, z, 512);
    close(fd);
    return 0;
}

#define XHASH "73cb3858a687a8494ca3323053016282f3dad39d42cf62ca4e79dda2aac7d9ac"   /* sha256("x\n") */

static char manifest_buf[2048];

/* Manifest pentru pachetul t1 cu un fisier opt/t1/f ("x\n"); `extra` = linii in plus la nivel superior, `file` = calea din [files] */
static const char *mk_manifest(const char *extra, const char *file, int with_sig){
    snprintf(manifest_buf, sizeof(manifest_buf),
             "format = 1\nname = \"t1\"\nversion = \"0.1\"\n%s\n%s\n[files]\n\"%s\" = { sha256 = \"" XHASH "\", size = 2, mode = 0o644 }\n",
             extra, with_sig ? "signature = \"\"" : "", file);
    return manifest_buf;
}

/* ---- teste ---- */

static void test_basic(void){
    rm_root();
    CHECK(runv("/bin/ypkg", (char *[]){"ypkg", "--help", NULL}) == 0 && strncmp(out, "uz: ypkg", 8) == 0);
    CHECK(runv("/bin/ypkg", (char *[]){"ypkg", NULL}) == 2);
    CHECK(ypkg("frumos", NULL, NULL) == 2);
    CHECK(ypkg("list", NULL, NULL) == 0 && out[0] == 0);              // fara baza de date = nimic instalat

    CHECK(ypkg("verify", HELLO, NULL) == 0 && has(out, "ok: hello 1.0.0 (2 fisiere)"));
    CHECK(ypkg("info", HELLO, NULL) == 0 && has(out, "Nume:       hello") && has(out, "Versiune:   1.0.0") &&
          has(out, "Intrare:    /opt/hello/bin/hello") && has(out, "(nesemnat)") && has(out, "Dependente: -") &&
          has(out, "/opt/hello/share/hello.txt  (44 B, 0644)") && has(out, "/opt/hello/bin/hello  ("));

    // hello-extra cere hello >= 1.0: fara hello nu se instaleaza si nu lasa nimic in urma
    CHECK(ypkg("install", EXTRA, NULL) == 1 && has(err, "dependenta lipsa: hello >= 1.0"));
    CHECK(!exists(ROOT "/opt/hello-extra") && !exists(ROOT "/var/lib/ypkg/hello-extra"));

    CHECK(ypkg("install", HELLO, NULL) == 0 && has(out, "instalat hello 1.0.0 (2 fisiere)"));
    struct stat st;
    CHECK(stat(ROOT "/opt/hello/bin/hello", &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0111) == 0111);
    CHECK(stat(ROOT "/opt/hello/share/hello.txt", &st) == 0 && (st.st_mode & 0111) == 0);
    CHECK(file_is(ROOT "/opt/hello/share/hello.txt", "Acest fisier vine din pachetul hello.yokai.\n"));
    CHECK(exists(ROOT "/var/lib/ypkg/hello/manifest.toml") && exists(ROOT "/var/lib/ypkg/hello/files"));
    // politica de capabilitati pentru kernel: o linie pe executabil, din permisiunile cap.* (aici cap.net)
    CHECK(file_is(ROOT "/var/lib/ypkg/exec.caps", "/opt/hello/bin/hello net hello\n"));

    // binarul instalat ruleaza (direct si prin `ypkg run`)
    CHECK(runv(ROOT "/opt/hello/bin/hello", (char *[]){"hello", "a", "b c", NULL}) == 0 &&
          strcmp(out, "salut de la hello 1.0.0 (pachet .yokai), argc=3\n  arg 1 = \"a\"\n  arg 2 = \"b c\"\n") == 0);
    CHECK(ypkg("run", "hello", "xyz") == 0 && has(out, "argc=2") && has(out, "arg 1 = \"xyz\""));
    CHECK(ypkg("run", "nuexista", NULL) == 1);

    CHECK(ypkg("list", NULL, NULL) == 0 && has(out, "hello ") && has(out, "1.0.0") && has(out, "Program de proba"));
    CHECK(ypkg("info", "hello", NULL) == 0 && has(out, "Nume:       hello") && has(out, "Permisiuni: tty"));
    CHECK(ypkg("info", "nuexista", NULL) == 1);
    CHECK(ypkg("install", HELLO, NULL) == 1 && has(err, "deja instalat"));

    CHECK(ypkg("install", EXTRA, NULL) == 0 && has(out, "instalat hello-extra 2.1.0"));
    CHECK(file_is(ROOT "/opt/hello-extra/share/extra.txt", "Date din pachetul hello-extra 2.1.0.\n"));
    CHECK(ypkg("list", NULL, NULL) == 0 && strstr(out, "hello ") && strstr(out, "hello-extra") &&
          strstr(out, "hello ") < strstr(out, "hello-extra"));           // sortat

    // hello e cerut de hello-extra
    CHECK(ypkg("remove", "hello", NULL) == 1 && has(err, "il cer hello-extra"));
    CHECK(exists(ROOT "/opt/hello/bin/hello"));
    CHECK(file_is(ROOT "/var/lib/ypkg/exec.caps", "/opt/hello/bin/hello net hello\n"));   // hello-extra nu are executabile
    CHECK(ypkg("remove", "hello-extra", NULL) == 0 && has(out, "scos hello-extra (1 fisiere)"));
    CHECK(!exists(ROOT "/opt/hello-extra/share/extra.txt") && !exists(ROOT "/opt/hello-extra") && !exists(ROOT "/var/lib/ypkg/hello-extra"));
    CHECK(ypkg("remove", "hello-extra", NULL) == 1 && has(err, "nu e instalat"));
    CHECK(ypkg("remove", "../etc", NULL) == 2);
    CHECK(ypkg("remove", "hello", NULL) == 0 && has(out, "scos hello (2 fisiere)"));
    CHECK(!exists(ROOT "/var/lib/ypkg/exec.caps"));                     // nu mai ramane nicio linie
    CHECK(!exists(ROOT "/opt/hello") && !exists(ROOT "/opt/hello/bin/hello") && !exists(ROOT "/var/lib/ypkg/hello"));
    CHECK(ypkg("list", NULL, NULL) == 0 && out[0] == 0);

    // -f scoate chiar daca e cerut
    CHECK(ypkg("install", HELLO, NULL) == 0 && ypkg("install", EXTRA, NULL) == 0);
    CHECK(runv("/bin/ypkg", (char *[]){"ypkg", "-r", ROOT, "-f", "remove", "hello", NULL}) == 0);
    CHECK(ypkg("remove", "hello-extra", NULL) == 0);
    CHECK(ypkg("list", NULL, NULL) == 0 && out[0] == 0);
}

static void test_tamper(void){
    size_t n;
    char *pkg = slurp_file(HELLO, &n);
    CHECK(pkg && n > 2048);
    if(!pkg){
        return;
    }
    // un octet schimbat in continutul unui fisier
    char *at = memmem(pkg, n, "Acest fisier", 12);
    CHECK(at != NULL);
    if(at){
        at[0] = 'a';
        int fd = open("/tmp/bad1.yokai", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        write(fd, pkg, n);
        close(fd);
        CHECK(ypkg("verify", "/tmp/bad1.yokai", NULL) == 1 && has(err, "SHA-256 gresit pentru opt/hello/share/hello.txt"));
        CHECK(ypkg("install", "/tmp/bad1.yokai", NULL) == 1 && has(err, "SHA-256 gresit"));
        CHECK(!exists(ROOT "/opt/hello") && !exists(ROOT "/var/lib/ypkg/hello"));
        at[0] = 'A';
    }
    // un caracter din hash-ul din manifest
    char *h = memmem(pkg, n, "sha256 = \"", 10);
    CHECK(h != NULL);
    if(h){
        h[10] = h[10] == '0' ? '1' : '0';
        int fd = open("/tmp/bad2.yokai", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        write(fd, pkg, n);
        close(fd);
        CHECK(ypkg("install", "/tmp/bad2.yokai", NULL) == 1 && has(err, "SHA-256 gresit"));
        CHECK(!exists(ROOT "/opt/hello"));
    }
    // arhiva taiata
    int fd = open("/tmp/bad3.yokai", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    write(fd, pkg, n - 1500);
    close(fd);
    CHECK(ypkg("install", "/tmp/bad3.yokai", NULL) == 1 && has(err, "ypkg:"));
    CHECK(!exists(ROOT "/opt/hello") && !exists(ROOT "/var/lib/ypkg/hello"));
    free(pkg);
    CHECK(ypkg("install", "/tmp/nu-exista.yokai", NULL) == 1);
    CHECK(ypkg("install", "/etc/motd", NULL) == 1);                     // nu e tar valid
    unlink("/tmp/bad1.yokai");
    unlink("/tmp/bad2.yokai");
    unlink("/tmp/bad3.yokai");
}

static int try_pkg(const char *manifest, const TEnt *ents, int n){
    mkpkg("/tmp/t1.yokai", manifest, ents, n);
    return ypkg("install", "/tmp/t1.yokai", NULL);
}

static void test_crafted(void){
    static const TEnt good[] = {{"opt/t1/f", '0', "x\n"}};
    CHECK(try_pkg(mk_manifest("", "opt/t1/f", 1), good, 1) == 0 && file_is(ROOT "/opt/t1/f", "x\n"));
    CHECK(ypkg("remove", "t1", NULL) == 0 && !exists(ROOT "/opt/t1"));

    // fara semnatura (camp lipsa), lipseste din manifest -> refuzat; gol -> acceptat (semnatura vine in T8.5)
    CHECK(try_pkg(mk_manifest("", "opt/t1/f", 0), good, 1) == 1 && has(err, "signature"));
    CHECK(try_pkg(mk_manifest("signature = \"ab12\"", "opt/t1/f", 0), good, 1) == 0);
    CHECK(ypkg("info", "t1", NULL) == 0 && has(out, "Semnatura:  ab12"));
    CHECK(ypkg("remove", "t1", NULL) == 0);

    // cai ostile
    static const TEnt trav[] = {{"opt/../../evil", '0', "x\n"}};
    CHECK(try_pkg(mk_manifest("signature = \"\"", "opt/t1/f", 0), trav, 1) == 1 && has(err, "cale nepermisa"));
    CHECK(!exists("/evil") && !exists("/tmp/evil"));
    static const TEnt abs[] = {{"/opt/t1/f", '0', "x\n"}};
    CHECK(try_pkg(mk_manifest("signature = \"\"", "opt/t1/f", 0), abs, 1) == 1 && has(err, "cale nepermisa"));
    static const TEnt top[] = {{"tmp/t1f", '0', "x\n"}};
    CHECK(try_pkg(mk_manifest("signature = \"\"", "tmp/t1f", 0), top, 1) == 1 && has(err, "cale nepermisa"));
    static const TEnt dbp[] = {{"var/lib/ypkg/t1/manifest.toml", '0', "x\n"}};
    CHECK(try_pkg(mk_manifest("signature = \"\"", "var/lib/ypkg/t1/manifest.toml", 0), dbp, 1) == 1 && has(err, "cale nepermisa"));
    CHECK(!exists(ROOT "/var/lib/ypkg/t1"));

    // intrari care nu sunt in manifest, legaturi simbolice, fisiere lipsa
    static const TEnt extra[] = {{"opt/t1/f", '0', "x\n"}, {"opt/t1/g", '0', "x\n"}};
    CHECK(try_pkg(mk_manifest("signature = \"\"", "opt/t1/f", 0), extra, 2) == 1 && has(err, "nu e in manifest"));
    static const TEnt sym[] = {{"opt/t1/f", '2', NULL}};
    CHECK(try_pkg(mk_manifest("signature = \"\"", "opt/t1/f", 0), sym, 1) == 1 && has(err, "nesuportat"));
    CHECK(try_pkg(mk_manifest("signature = \"\"", "opt/t1/f", 0), good, 0) == 1 && has(err, "lipsa din arhiva"));
    static const TEnt wrong[] = {{"opt/t1/f", '0', "y\n"}};
    CHECK(try_pkg(mk_manifest("signature = \"\"", "opt/t1/f", 0), wrong, 1) == 1 && has(err, "SHA-256 gresit"));
    CHECK(!exists(ROOT "/opt/t1") && !exists(ROOT "/var/lib/ypkg/t1"));

    // entry care nu e in pachet; manifest stricat
    CHECK(try_pkg(mk_manifest("signature = \"\"\nentry = \"/opt/t1/nu\"", "opt/t1/f", 0), good, 1) == 1 && has(err, "entry"));
    CHECK(try_pkg("format = 2\nname = \"t1\"\nversion = \"1\"\nsignature = \"\"\n", good, 1) == 1 && has(err, "format"));
    CHECK(try_pkg("format = 1\nname = \"T 1\"\nversion = \"1\"\nsignature = \"\"\n", good, 1) == 1 && has(err, "name"));
    CHECK(try_pkg("format = 1\nname = \"t1\"\nversion = \"1\"\nsignature = \"\"\nx = [\n", good, 1) == 1 && has(err, "linia"));
    CHECK(try_pkg("format = 1\nname = \"t1\"\nversion = \"1\"\nsignature = \"\"\ndeps = [\"a b c\"]\n", good, 1) == 1 && has(err, "dependenta invalida"));
    static const TEnt only[] = {{"opt/t1/f", '0', "x\n"}};
    // arhiva fara manifest.toml pe primul loc
    {
        const char *mf = mk_manifest("signature = \"\"", "opt/t1/f", 0);
        mkpkg("/tmp/t1.yokai", mf, only, 1);
        size_t n, skip = 512 + (strlen(mf) + 511) / 512 * 512;
        char *b = slurp_file("/tmp/t1.yokai", &n);
        int fd = open("/tmp/t2.yokai", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        write(fd, b + skip, n - skip);
        close(fd);
        free(b);
        CHECK(ypkg("install", "/tmp/t2.yokai", NULL) == 1 && has(err, "manifest.toml"));
        unlink("/tmp/t2.yokai");
    }
    CHECK(!exists(ROOT "/opt/t1"));

    // conflict cu un fisier existent; nimic nu se schimba
    mkdir(ROOT "/opt", 0755);
    mkdir(ROOT "/opt/t1", 0755);
    int fd = open(ROOT "/opt/t1/f", O_WRONLY | O_CREAT, 0644);
    write(fd, "al meu\n", 7);
    close(fd);
    CHECK(try_pkg(mk_manifest("signature = \"\"", "opt/t1/f", 0), good, 1) == 1 && has(err, "conflict"));
    CHECK(file_is(ROOT "/opt/t1/f", "al meu\n") && !exists(ROOT "/var/lib/ypkg/t1"));
    unlink(ROOT "/opt/t1/f");
    rmdir(ROOT "/opt/t1");

    // directoare din arhiva + un fisier care incearca sa treaca printr-un fisier existent -> rollback complet
    static const TEnt blocked[] = {{"opt/t1", '5', NULL}, {"opt/t1/f", '0', "x\n"}, {"opt/t1/f/g", '0', "x\n"}};
    static const char mf_blocked[] =
        "format = 1\nname = \"t1\"\nversion = \"0.1\"\nsignature = \"\"\n[files]\n"
        "\"opt/t1/f\" = { sha256 = \"" XHASH "\", size = 2, mode = 0o644 }\n"
        "\"opt/t1/f/g\" = { sha256 = \"" XHASH "\", size = 2, mode = 0o644 }\n";
    CHECK(try_pkg(mf_blocked, blocked, 3) == 1);
    CHECK(!exists(ROOT "/opt/t1/f") && !exists(ROOT "/opt/t1") && !exists(ROOT "/var/lib/ypkg/t1"));
    unlink("/tmp/t1.yokai");
}

/* Constrangeri de versiune: t1 depinde de "hello <op> <versiune>"; hello instalat = 1.0.0 */
static void test_versions(void){
    static const struct { const char *spec; int ok; } cases[] = {
        {"hello", 1}, {"hello >= 1.0", 1}, {"hello >= 1.0.0", 1}, {"hello > 1.0.0", 0}, {"hello = 1.0.0", 1},
        {"hello == 1", 1}, {"hello < 1.0.1", 1}, {"hello <= 0.9", 0}, {"hello >= 1.0.0.1", 0}, {"hello >= 1.10", 0},
        {"hello < 1.10", 1}, {"hello >= 0.99.99", 1}, {"hello >= 2", 0}, {"altceva", 0}, {"hello>=1", 1},
    };
    static const TEnt good[] = {{"opt/t1/f", '0', "x\n"}};
    CHECK(ypkg("install", HELLO, NULL) == 0);
    for(unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++){
        char extra[160];
        snprintf(extra, sizeof(extra), "signature = \"\"\ndeps = [\"%s\"]", cases[i].spec);
        int rc = try_pkg(mk_manifest(extra, "opt/t1/f", 0), good, 1);
        checks++;
        if(rc != (cases[i].ok ? 0 : 1)){
            failed++;
            fprintf(stderr, "ypkgtest: FAIL versiune \"%s\" rc=%d (%s)\n", cases[i].spec, rc, err);
        }
        if(rc == 0){
            ypkg("remove", "t1", NULL);
        }
    }
    CHECK(ypkg("remove", "hello", NULL) == 0);
    unlink("/tmp/t1.yokai");
}

static void test_real_root(void){
    // fara -r: radacina "/" e initrd (read-only), instalarea esueaza curat
    char *av[] = {"ypkg", "install", HELLO, NULL};
    int rc = runv("/bin/ypkg", av);
    CHECK(rc == 1 && has(err, "ypkg:"));
    CHECK(!exists("/opt/hello"));
}

int main(void){
    test_basic();
    test_tamper();
    test_crafted();
    test_versions();
    test_real_root();
    rm_root();
    printf("ypkgtest: %d verificari, %d esuate\n", checks, failed);
    if(failed){
        return 1;
    }
    printf("[OK] ypkgtest\n");
    return 0;
}

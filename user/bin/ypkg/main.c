/* ypkg: instaleaza/sterge pachete .yokai (tar ustar + manifest.toml cu SHA-256 per fisier). Vezi docs/PACKAGE_FORMAT.md
 *   ypkg [-r radacina] [-f] install <fisier.yokai> | remove <nume> | list | info <nume|fisier.yokai> | run <nume> [arg...] | verify <fisier.yokai>
 * Baza de date: <radacina>/var/lib/ypkg/<nume>/{manifest.toml,files}. Radacina implicita: "/" (sau $YPKG_ROOT) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include "ypkg.h"

#define DB_REL "var/lib/ypkg"
#define MANIFEST_MAX (1024 * 1024)

static const char *root = "";   /* fara '/' la sfarsit; "" = "/" */
static int force;

typedef struct {
    char **v;
    int n, cap;
} List;

static int list_push(List *l, const char *s){
    if(l->n == l->cap){
        int cap = l->cap ? l->cap * 2 : 16;
        char **t = realloc(l->v, (size_t)cap * sizeof(*t));
        if(!t){
            return -1;
        }
        l->v = t;
        l->cap = cap;
    }
    char *d = strdup(s);
    if(!d){
        return -1;
    }
    l->v[l->n++] = d;
    return 0;
}

static void list_free(List *l){
    for(int i = 0; i < l->n; i++){
        free(l->v[i]);
    }
    free(l->v);
    memset(l, 0, sizeof(*l));
}

static void rpath(char *out, size_t n, const char *rel){
    snprintf(out, n, "%s/%s", root, rel);
}

static void dbpath(char *out, size_t n, const char *name, const char *file){
    if(!name){
        snprintf(out, n, "%s/" DB_REL, root);
    } else if(!file){
        snprintf(out, n, "%s/" DB_REL "/%s", root, name);
    } else {
        snprintf(out, n, "%s/" DB_REL "/%s/%s", root, name, file);
    }
}

static char *read_all(const char *path, size_t *len, size_t max){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return NULL;
    }
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap + 1);
    for(;;){
        if(!buf){
            close(fd);
            errno = ENOMEM;
            return NULL;
        }
        if(n == cap){
            cap *= 2;
            char *t = realloc(buf, cap + 1);
            if(!t){
                free(buf);
                buf = NULL;
                continue;
            }
            buf = t;
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if(r < 0){
            int e = errno;
            free(buf);
            close(fd);
            errno = e;
            return NULL;
        }
        if(r == 0){
            break;
        }
        n += (size_t)r;
        if(n > max){
            free(buf);
            close(fd);
            errno = EFBIG;
            return NULL;
        }
    }
    close(fd);
    buf[n] = 0;
    *len = n;
    return buf;
}

static int write_file(const char *path, const void *data, size_t len, unsigned mode){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if(fd < 0){
        return -1;
    }
    const char *p = data;
    while(len){
        ssize_t w = write(fd, p, len);
        if(w <= 0){
            int e = errno ? errno : EIO;
            close(fd);
            errno = e;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return close(fd);
}

/* mkdir -p; directoarele create acum se adauga in `created` (cai complete) */
static int ensure_dir(const char *path, List *created){
    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    for(char *p = buf + 1; ; p++){
        if(*p == '/' || *p == 0){
            char save = *p;
            *p = 0;
            if(mkdir(buf, 0755) == 0){
                if(created && list_push(created, buf) < 0){
                    return -1;
                }
            } else if(errno == EEXIST){
                struct stat st;
                if(stat(buf, &st) < 0 || !S_ISDIR(st.st_mode)){
                    errno = ENOTDIR;
                    return -1;
                }
            } else {
                return -1;
            }
            *p = save;
            if(!save){
                return 0;
            }
        }
    }
}

/* Directoare permise pentru continutul pachetelor (nu /proc /dev /tmp /run /boot ...), si niciodata baza de date */
static int allowed_path(const char *rel){
    static const char *const tops[] = {"bin", "sbin", "lib", "usr", "etc", "opt", "var", "home"};
    const char *slash = strchr(rel, '/');
    size_t n = slash ? (size_t)(slash - rel) : strlen(rel);
    int ok = 0;
    for(unsigned i = 0; i < sizeof(tops) / sizeof(tops[0]); i++){
        if(strlen(tops[i]) == n && strncmp(rel, tops[i], n) == 0){
            ok = 1;
        }
    }
    if(!ok){
        return 0;
    }
    size_t dl = strlen(DB_REL);
    if(strncmp(rel, DB_REL, dl) == 0 && (rel[dl] == 0 || rel[dl] == '/')){
        return 0;
    }
    return 1;
}

/* ---- pachet ---- */

typedef struct {
    int fd;
    Manifest m;
    char *raw;        /* manifest.toml in forma lui originala */
    size_t rawlen;
    off_t data_off;   /* de aici incep intrarile cu continut */
} Pkg;

static void pkg_close(Pkg *p){
    if(p->fd >= 0){
        close(p->fd);
    }
    manifest_free(&p->m);
    free(p->raw);
    memset(p, 0, sizeof(*p));
    p->fd = -1;
}

static int pkg_open(Pkg *p, const char *file, char *err, size_t errsz){
    memset(p, 0, sizeof(*p));
    p->fd = open(file, O_RDONLY);
    if(p->fd < 0){
        snprintf(err, errsz, "%s: %s", file, strerror(errno));
        return -1;
    }
    TarEnt e;
    char msg[160];
    int rc = tar_next(p->fd, &e, msg, sizeof(msg));
    if(rc <= 0){
        snprintf(err, errsz, "%s: %s", file, rc == 0 ? "arhiva goala" : msg);
        goto bad;
    }
    if(strcmp(e.name, "manifest.toml") != 0 || e.type != '0'){
        snprintf(err, errsz, "%s: prima intrare trebuie sa fie manifest.toml", file);
        goto bad;
    }
    if(e.size <= 0 || e.size > MANIFEST_MAX){
        snprintf(err, errsz, "%s: manifest.toml are marime invalida", file);
        goto bad;
    }
    p->rawlen = (size_t)e.size;
    p->raw = malloc(p->rawlen + 1);
    if(!p->raw){
        snprintf(err, errsz, "memorie insuficienta");
        goto bad;
    }
    size_t got = 0;
    while(got < p->rawlen){
        ssize_t r = read(p->fd, p->raw + got, p->rawlen - got);
        if(r <= 0){
            snprintf(err, errsz, "%s: manifest.toml trunchiat", file);
            goto bad;
        }
        got += (size_t)r;
    }
    p->raw[p->rawlen] = 0;
    long long pad = (512 - e.size % 512) % 512;
    if(pad && lseek(p->fd, (off_t)pad, SEEK_CUR) < 0){
        snprintf(err, errsz, "%s: %s", file, strerror(errno));
        goto bad;
    }
    p->data_off = lseek(p->fd, 0, SEEK_CUR);
    if(manifest_parse(p->raw, p->rawlen, &p->m, msg, sizeof(msg)) < 0){
        snprintf(err, errsz, "%s: %s", file, msg);
        goto bad;
    }
    if(p->m.entry){
        const char *rel = p->m.entry[0] == '/' ? p->m.entry + 1 : p->m.entry;
        if(!manifest_find(&p->m, rel)){
            snprintf(err, errsz, "%s: entry (%s) nu e un fisier din pachet", file, p->m.entry);
            goto bad;
        }
    }
    for(int i = 0; i < p->m.nfiles; i++){
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s", p->m.files[i].path);
        if(path_normalize(tmp) < 0 || strcmp(tmp, p->m.files[i].path) != 0 || !allowed_path(tmp)){
            snprintf(err, errsz, "%s: cale nepermisa in [files]: %s", file, p->m.files[i].path);
            goto bad;
        }
    }
    for(int i = 0; i < p->m.ndeps; i++){
        char n[80], op[4], v[80];
        if(dep_split(p->m.deps[i], n, sizeof(n), op, v, sizeof(v)) < 0){
            snprintf(err, errsz, "%s: dependenta invalida: \"%s\"", file, p->m.deps[i]);
            goto bad;
        }
    }
    return 0;
bad:
    pkg_close(p);
    return -1;
}

/* Trecerea 1: fiecare intrare tar trebuie sa fie in manifest, cu marimea si SHA-256 declarate; nimic in plus, nimic lipsa */
static int pkg_verify(Pkg *p, char *err, size_t errsz){
    if(lseek(p->fd, p->data_off, SEEK_SET) < 0){
        snprintf(err, errsz, "lseek: %s", strerror(errno));
        return -1;
    }
    char *seen = calloc((size_t)(p->m.nfiles ? p->m.nfiles : 1), 1);
    if(!seen){
        snprintf(err, errsz, "memorie insuficienta");
        return -1;
    }
    TarEnt e;
    char msg[160];
    int rc;
    while((rc = tar_next(p->fd, &e, msg, sizeof(msg))) > 0){
        if(path_normalize(e.name) < 0 || !allowed_path(e.name)){
            snprintf(err, errsz, "cale nepermisa in arhiva: %s", e.name);
            goto bad;
        }
        if(e.type == '5'){
            if(e.size){
                snprintf(err, errsz, "director cu date: %s", e.name);
                goto bad;
            }
            continue;
        }
        if(e.type != '0'){
            snprintf(err, errsz, "tip de intrare nesuportat (doar fisiere si directoare): %s", e.name);
            goto bad;
        }
        const PkgFile *f = manifest_find(&p->m, e.name);
        if(!f){
            snprintf(err, errsz, "fisier care nu e in manifest: %s", e.name);
            goto bad;
        }
        int idx = (int)(f - p->m.files);
        if(seen[idx]){
            snprintf(err, errsz, "fisier repetat in arhiva: %s", e.name);
            goto bad;
        }
        seen[idx] = 1;
        if(e.size != f->size){
            snprintf(err, errsz, "marime diferita de manifest: %s", e.name);
            goto bad;
        }
        Sha256 s;
        char hex[65];
        sha256_init(&s);
        if(tar_stream(p->fd, &e, &s, -1) < 0){
            snprintf(err, errsz, "arhiva trunchiata la %s", e.name);
            goto bad;
        }
        sha256_final(&s, hex);
        if(strcmp(hex, f->sha256) != 0){
            snprintf(err, errsz, "SHA-256 gresit pentru %s (arhiva e corupta sau modificata)", e.name);
            goto bad;
        }
    }
    if(rc < 0){
        snprintf(err, errsz, "%s", msg);
        goto bad;
    }
    for(int i = 0; i < p->m.nfiles; i++){
        if(!seen[i]){
            snprintf(err, errsz, "fisier din manifest lipsa din arhiva: %s", p->m.files[i].path);
            goto bad;
        }
    }
    free(seen);
    return 0;
bad:
    free(seen);
    return -1;
}

/* ---- baza de date ---- */

static int is_installed(const char *name){
    char p[512];
    struct stat st;
    dbpath(p, sizeof(p), name, "manifest.toml");
    return stat(p, &st) == 0;
}

static int load_installed(const char *name, Manifest *m, char *err, size_t errsz){
    char p[512];
    size_t len;
    dbpath(p, sizeof(p), name, "manifest.toml");
    char *raw = read_all(p, &len, MANIFEST_MAX);
    if(!raw){
        snprintf(err, errsz, "%s: %s", name, errno == ENOENT ? "nu e instalat" : strerror(errno));
        return -1;
    }
    char msg[160];
    int rc = manifest_parse(raw, len, m, msg, sizeof(msg));
    free(raw);
    if(rc < 0){
        snprintf(err, errsz, "%s: baza de date corupta: %s", name, msg);
    }
    return rc;
}

static int name_cmp(const void *a, const void *b){
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Numele pachetelor instalate, sortate */
static int installed_names(List *l){
    char p[512];
    dbpath(p, sizeof(p), NULL, NULL);
    DIR *d = opendir(p);
    if(!d){
        return errno == ENOENT ? 0 : -1;
    }
    struct dirent *e;
    while((e = readdir(d))){
        if(e->d_name[0] == '.' || !is_installed(e->d_name)){
            continue;
        }
        if(list_push(l, e->d_name) < 0){
            closedir(d);
            return -1;
        }
    }
    closedir(d);
    qsort(l->v, (size_t)l->n, sizeof(char *), name_cmp);
    return 0;
}

static int check_deps(const Manifest *m, char *err, size_t errsz){
    for(int i = 0; i < m->ndeps; i++){
        char n[80], op[4], v[80], msg[160];
        dep_split(m->deps[i], n, sizeof(n), op, v, sizeof(v));
        Manifest dm;
        if(load_installed(n, &dm, msg, sizeof(msg)) < 0){
            snprintf(err, errsz, "dependenta lipsa: %s", m->deps[i]);
            return -1;
        }
        int ok = version_ok(dm.version, op, v);
        if(!ok){
            snprintf(err, errsz, "dependenta nesatisfacuta: %s (instalat: %s %s)", m->deps[i], n, dm.version);
        }
        manifest_free(&dm);
        if(!ok){
            return -1;
        }
    }
    return 0;
}

static void rollback(List *files, List *dirs){
    for(int i = files->n - 1; i >= 0; i--){
        unlink(files->v[i]);
    }
    for(int i = dirs->n - 1; i >= 0; i--){
        rmdir(dirs->v[i]);
    }
}

static int do_install(const char *file){
    Pkg p;
    char err[256];
    if(pkg_open(&p, file, err, sizeof(err)) < 0 || pkg_verify(&p, err, sizeof(err)) < 0){
        fprintf(stderr, "ypkg: %s\n", err);
        pkg_close(&p);
        return 1;
    }
    const Manifest *m = &p.m;
    int rc = 1;
    List files = {0}, dirs = {0};
    char path[1024], dbdir[512];

    if(is_installed(m->name)){
        fprintf(stderr, "ypkg: %s e deja instalat (scoate-l mai intai cu: ypkg remove %s)\n", m->name, m->name);
        goto out;
    }
    if(check_deps(m, err, sizeof(err)) < 0){
        fprintf(stderr, "ypkg: nu pot instala %s: %s\n", m->name, err);
        goto out;
    }
    for(int i = 0; i < m->nfiles; i++){
        struct stat st;
        rpath(path, sizeof(path), m->files[i].path);
        if(lstat(path, &st) == 0){
            fprintf(stderr, "ypkg: conflict: %s exista deja (nu apartine unui pachet instalat sau al altuia)\n", path);
            goto out;
        }
    }

    /* baza de date: radacina ei se creeaza daca lipseste */
    dbpath(dbdir, sizeof(dbdir), NULL, NULL);
    if(ensure_dir(dbdir, NULL) < 0){
        fprintf(stderr, "ypkg: %s: %s\n", dbdir, strerror(errno));
        goto out;
    }

    /* trecerea 2: extragere (SHA-256 se verifica din nou, arhiva ar putea fi schimbata intre treceri) */
    if(lseek(p.fd, p.data_off, SEEK_SET) < 0){
        fprintf(stderr, "ypkg: lseek: %s\n", strerror(errno));
        goto out;
    }
    TarEnt e;
    char msg[160];
    int r;
    while((r = tar_next(p.fd, &e, msg, sizeof(msg))) > 0){
        path_normalize(e.name);
        rpath(path, sizeof(path), e.name);
        if(e.type == '5'){
            if(ensure_dir(path, &dirs) < 0){
                fprintf(stderr, "ypkg: %s: %s\n", path, strerror(errno));
                goto undo;
            }
            continue;
        }
        const PkgFile *f = manifest_find(m, e.name);
        if(!f){
            fprintf(stderr, "ypkg: arhiva s-a schimbat in timpul instalarii (%s)\n", e.name);
            goto undo;
        }
        char parent[1024];
        snprintf(parent, sizeof(parent), "%s", path);
        char *slash = strrchr(parent, '/');
        if(slash && slash != parent){
            *slash = 0;
            if(ensure_dir(parent, &dirs) < 0){
                fprintf(stderr, "ypkg: %s: %s\n", parent, strerror(errno));
                goto undo;
            }
        }
        int out_fd = open(path, O_WRONLY | O_CREAT | O_EXCL, f->mode ? f->mode : 0644);
        if(out_fd < 0){
            fprintf(stderr, "ypkg: %s: %s\n", path, strerror(errno));
            goto undo;
        }
        if(list_push(&files, path) < 0){
            close(out_fd);
            unlink(path);
            fprintf(stderr, "ypkg: memorie insuficienta\n");
            goto undo;
        }
        Sha256 s;
        char hex[65];
        sha256_init(&s);
        int sr = tar_stream(p.fd, &e, &s, out_fd);
        int cr = close(out_fd);
        if(sr == -2 || cr < 0){
            fprintf(stderr, "ypkg: scriere in %s: %s\n", path, strerror(errno));
            goto undo;
        }
        sha256_final(&s, hex);
        if(sr < 0 || e.size != f->size || strcmp(hex, f->sha256) != 0){
            fprintf(stderr, "ypkg: arhiva s-a schimbat in timpul instalarii (%s)\n", e.name);
            goto undo;
        }
    }
    if(r < 0){
        fprintf(stderr, "ypkg: %s\n", msg);
        goto undo;
    }

    /* intrarea in baza de date; manifest.toml se scrie ultimul (el decide daca pachetul conteaza ca instalat) */
    {
        dbpath(dbdir, sizeof(dbdir), m->name, NULL);
        if(mkdir(dbdir, 0755) < 0){
            fprintf(stderr, "ypkg: %s: %s\n", dbdir, strerror(errno));
            goto undo;
        }
        size_t cap = 256;
        for(int i = 0; i < files.n; i++){
            cap += strlen(files.v[i]) + 4;
        }
        for(int i = 0; i < dirs.n; i++){
            cap += strlen(dirs.v[i]) + 4;
        }
        char *lst = malloc(cap);
        size_t n = 0;
        if(!lst){
            rmdir(dbdir);
            goto undo;
        }
        size_t rl = strlen(root);
        /* in lista se pastreaza caile relative la radacina */
        for(int i = 0; i < files.n; i++){
            n += (size_t)snprintf(lst + n, cap - n, "F %s\n", files.v[i] + rl + 1);
        }
        for(int i = 0; i < dirs.n; i++){
            n += (size_t)snprintf(lst + n, cap - n, "D %s\n", dirs.v[i] + rl + 1);
        }
        char fp[512];
        dbpath(fp, sizeof(fp), m->name, "files");
        int w1 = write_file(fp, lst, n, 0644);
        free(lst);
        int e1 = errno;
        dbpath(fp, sizeof(fp), m->name, "manifest.toml");
        int w2 = w1 < 0 ? -1 : write_file(fp, p.raw, p.rawlen, 0644);
        if(w1 < 0 || w2 < 0){
            fprintf(stderr, "ypkg: scriere in baza de date: %s\n", strerror(w1 < 0 ? e1 : errno));
            dbpath(fp, sizeof(fp), m->name, "files");
            unlink(fp);
            dbpath(fp, sizeof(fp), m->name, "manifest.toml");
            unlink(fp);
            rmdir(dbdir);
            goto undo;
        }
    }
    printf("instalat %s %s (%d fisiere)\n", m->name, m->version, m->nfiles);
    rc = 0;
    goto out;
undo:
    rollback(&files, &dirs);
out:
    list_free(&files);
    list_free(&dirs);
    pkg_close(&p);
    return rc;
}

static int dependents(const char *name, List *out){
    List all = {0};
    if(installed_names(&all) < 0){
        return -1;
    }
    for(int i = 0; i < all.n; i++){
        Manifest m;
        char msg[160];
        if(strcmp(all.v[i], name) == 0 || load_installed(all.v[i], &m, msg, sizeof(msg)) < 0){
            continue;
        }
        for(int d = 0; d < m.ndeps; d++){
            char n[80], op[4], v[80];
            if(dep_split(m.deps[d], n, sizeof(n), op, v, sizeof(v)) == 0 && strcmp(n, name) == 0){
                list_push(out, all.v[i]);
                break;
            }
        }
        manifest_free(&m);
    }
    list_free(&all);
    return 0;
}

static int do_remove(const char *name){
    if(!valid_name(name)){
        fprintf(stderr, "ypkg: nume invalid: %s\n", name);
        return 2;
    }
    if(!is_installed(name)){
        fprintf(stderr, "ypkg: %s nu e instalat\n", name);
        return 1;
    }
    List deps = {0};
    dependents(name, &deps);
    if(deps.n && !force){
        fprintf(stderr, "ypkg: nu pot scoate %s: il cer", name);
        for(int i = 0; i < deps.n; i++){
            fprintf(stderr, "%s%s", i ? ", " : " ", deps.v[i]);
        }
        fprintf(stderr, " (-f ca sa-l scoti oricum)\n");
        list_free(&deps);
        return 1;
    }
    list_free(&deps);
    char p[512];
    size_t len;
    dbpath(p, sizeof(p), name, "files");
    char *lst = read_all(p, &len, 64 * 1024 * 1024);
    if(!lst){
        fprintf(stderr, "ypkg: %s: %s\n", p, strerror(errno));
        return 1;
    }
    /* nu mai conteaza ca instalat de acum; o intrerupere lasa doar lista de fisiere in urma */
    dbpath(p, sizeof(p), name, "manifest.toml");
    unlink(p);
    List dirs = {0};
    int removed = 0, rc = 0;
    for(char *line = lst; *line; ){
        char *nl = strchr(line, '\n');
        if(nl){
            *nl = 0;
        }
        if(line[0] && line[1] == ' '){
            char path[1024];
            rpath(path, sizeof(path), line + 2);
            if(line[0] == 'F'){
                if(unlink(path) == 0){
                    removed++;
                } else if(errno != ENOENT){
                    fprintf(stderr, "ypkg: %s: %s\n", path, strerror(errno));
                    rc = 1;
                }
            } else if(line[0] == 'D'){
                list_push(&dirs, path);
            }
        }
        if(!nl){
            break;
        }
        line = nl + 1;
    }
    for(int i = dirs.n - 1; i >= 0; i--){
        rmdir(dirs.v[i]);   /* ramane daca mai are continut (alt pachet sau fisiere ale utilizatorului) */
    }
    list_free(&dirs);
    free(lst);
    dbpath(p, sizeof(p), name, "files");
    unlink(p);
    dbpath(p, sizeof(p), name, NULL);
    rmdir(p);
    printf("scos %s (%d fisiere)\n", name, removed);
    return rc;
}

static int do_list(void){
    List l = {0};
    if(installed_names(&l) < 0){
        fprintf(stderr, "ypkg: baza de date: %s\n", strerror(errno));
        return 1;
    }
    for(int i = 0; i < l.n; i++){
        Manifest m;
        char msg[160];
        if(load_installed(l.v[i], &m, msg, sizeof(msg)) < 0){
            fprintf(stderr, "ypkg: %s\n", msg);
            continue;
        }
        printf("%-20s %-12s %s\n", m.name, m.version, m.description ? m.description : "");
        manifest_free(&m);
    }
    list_free(&l);
    return 0;
}

static void print_join(const char *label, char **v, int n){
    printf("%-12s", label);
    if(n == 0){
        printf("-");
    }
    for(int i = 0; i < n; i++){
        printf("%s%s", i ? ", " : "", v[i]);
    }
    printf("\n");
}

static void print_info(const Manifest *m){
    printf("%-12s%s\n", "Nume:", m->name);
    printf("%-12s%s\n", "Versiune:", m->version);
    printf("%-12s%s\n", "Descriere:", m->description ? m->description : "-");
    printf("%-12s%s\n", "Intrare:", m->entry ? m->entry : "-");
    print_join("Dependente:", m->deps, m->ndeps);
    print_join("Permisiuni:", m->perms, m->nperms);
    printf("%-12s%s\n", "Semnatura:", m->signature[0] ? m->signature : "(nesemnat)");
    printf("%-12s%d\n", "Fisiere:", m->nfiles);
    for(int i = 0; i < m->nfiles; i++){
        printf("  /%s  (%lld B, %04o)\n", m->files[i].path, m->files[i].size, m->files[i].mode);
    }
}

static int do_info(const char *what){
    char err[256];
    struct stat st;
    if(strchr(what, '/') || (stat(what, &st) == 0 && S_ISREG(st.st_mode) && !valid_name(what))){
        Pkg p;
        if(pkg_open(&p, what, err, sizeof(err)) < 0){
            fprintf(stderr, "ypkg: %s\n", err);
            return 1;
        }
        print_info(&p.m);
        pkg_close(&p);
        return 0;
    }
    Manifest m;
    if(load_installed(what, &m, err, sizeof(err)) < 0){
        fprintf(stderr, "ypkg: %s\n", err);
        return 1;
    }
    print_info(&m);
    manifest_free(&m);
    return 0;
}

static int do_verify(const char *file){
    Pkg p;
    char err[256];
    if(pkg_open(&p, file, err, sizeof(err)) < 0 || pkg_verify(&p, err, sizeof(err)) < 0){
        fprintf(stderr, "ypkg: %s\n", err);
        pkg_close(&p);
        return 1;
    }
    printf("ok: %s %s (%d fisiere)\n", p.m.name, p.m.version, p.m.nfiles);
    pkg_close(&p);
    return 0;
}

static int do_run(const char *name, char **args){
    Manifest m;
    char err[256];
    if(load_installed(name, &m, err, sizeof(err)) < 0){
        fprintf(stderr, "ypkg: %s\n", err);
        return 1;
    }
    if(!m.entry){
        fprintf(stderr, "ypkg: %s nu are intrare (entry)\n", name);
        manifest_free(&m);
        return 1;
    }
    char path[1024];
    rpath(path, sizeof(path), m.entry[0] == '/' ? m.entry + 1 : m.entry);
    manifest_free(&m);
    char *argv[32];
    int n = 0;
    argv[n++] = path;
    for(; args && *args && n < 31; args++){
        argv[n++] = *args;
    }
    argv[n] = NULL;
    execv(path, argv);
    fprintf(stderr, "ypkg: %s: %s\n", path, strerror(errno));
    return 127;
}

static int usage(int rc){
    fprintf(rc ? stderr : stdout, "uz: ypkg [-r radacina] [-f] install FISIER.yokai | remove NUME | list | info NUME|FISIER | run NUME [arg...] | verify FISIER\n");
    return rc;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        return usage(0);
    }
    const char *env = getenv("YPKG_ROOT");
    static char rootbuf[512];
    if(env && *env){
        snprintf(rootbuf, sizeof(rootbuf), "%s", env);
    }
    int i = 1;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++){
        if(strcmp(argv[i], "-f") == 0){
            force = 1;
        } else if((strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--root") == 0) && i + 1 < argc){
            snprintf(rootbuf, sizeof(rootbuf), "%s", argv[++i]);
        } else {
            fprintf(stderr, "ypkg: optiune necunoscuta: %s\n", argv[i]);
            return 2;
        }
    }
    size_t rl = strlen(rootbuf);
    while(rl && rootbuf[rl - 1] == '/'){
        rootbuf[--rl] = 0;
    }
    if(rl && rootbuf[0] != '/'){
        fprintf(stderr, "ypkg: radacina trebuie sa fie o cale absoluta\n");
        return 2;
    }
    root = rootbuf;
    if(i >= argc){
        return usage(2);
    }
    const char *cmd = argv[i++];
    int rest = argc - i;
    if(strcmp(cmd, "install") == 0 && rest == 1){
        return do_install(argv[i]);
    }
    if(strcmp(cmd, "remove") == 0 && rest == 1){
        return do_remove(argv[i]);
    }
    if(strcmp(cmd, "list") == 0 && rest == 0){
        return do_list();
    }
    if(strcmp(cmd, "info") == 0 && rest == 1){
        return do_info(argv[i]);
    }
    if(strcmp(cmd, "verify") == 0 && rest == 1){
        return do_verify(argv[i]);
    }
    if(strcmp(cmd, "run") == 0 && rest >= 1){
        return do_run(argv[i], argv + i + 1);
    }
    return usage(2);
}

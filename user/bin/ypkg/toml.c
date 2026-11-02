/* manifest.toml: subset de TOML (string, intreg, lista de stringuri, tabel inline, tabelul [files]) + versiuni */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "ypkg.h"

typedef struct {
    const char *p, *end;
    int line;
    char *err;
    size_t errsz;
} Cur;

static int fail(Cur *c, const char *msg){
    snprintf(c->err, c->errsz, "manifest.toml, linia %d: %s", c->line, msg);
    return -1;
}

static int at_end(Cur *c){
    return c->p >= c->end;
}

static void skip_ws(Cur *c){
    while(!at_end(c) && (*c->p == ' ' || *c->p == '\t' || *c->p == '\r')){
        c->p++;
    }
}

static void skip_comment(Cur *c){
    if(!at_end(c) && *c->p == '#'){
        while(!at_end(c) && *c->p != '\n'){
            c->p++;
        }
    }
}

/* spatii, comentarii si linii goale */
static void skip_blank(Cur *c){
    for(;;){
        skip_ws(c);
        skip_comment(c);
        if(!at_end(c) && *c->p == '\n'){
            c->line++;
            c->p++;
        } else {
            return;
        }
    }
}

/* "..." (cu \" \\ \n \t \r) sau '...' */
static int parse_string(Cur *c, char **out){
    char q = *c->p++;
    size_t cap = 32, n = 0;
    char *s = malloc(cap);
    if(!s){
        return fail(c, "memorie insuficienta");
    }
    for(;;){
        if(at_end(c) || *c->p == '\n'){
            free(s);
            return fail(c, "sir neinchis");
        }
        char ch = *c->p++;
        if(ch == q){
            break;
        }
        if(q == '"' && ch == '\\'){
            if(at_end(c)){
                free(s);
                return fail(c, "sir neinchis");
            }
            char e = *c->p++;
            ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
            if(e != 'n' && e != 't' && e != 'r' && e != '"' && e != '\\'){
                free(s);
                return fail(c, "secventa de scapare necunoscuta");
            }
        } else if((unsigned char)ch < 0x20){
            free(s);
            return fail(c, "caracter de control intr-un sir");
        }
        if(n + 2 > cap){
            cap *= 2;
            char *t = realloc(s, cap);
            if(!t){
                free(s);
                return fail(c, "memorie insuficienta");
            }
            s = t;
        }
        s[n++] = ch;
    }
    s[n] = 0;
    *out = s;
    return 0;
}

static int parse_int(Cur *c, long long *out){
    const char *p = c->p;
    int neg = 0;
    if(p < c->end && (*p == '+' || *p == '-')){
        neg = *p == '-';
        p++;
    }
    int base = 10;
    if(p + 1 < c->end && p[0] == '0' && (p[1] == 'o' || p[1] == 'x' || p[1] == 'b')){
        base = p[1] == 'o' ? 8 : p[1] == 'x' ? 16 : 2;
        p += 2;
    }
    long long v = 0;
    int digits = 0;
    while(p < c->end){
        int d;
        if(*p >= '0' && *p <= '9'){
            d = *p - '0';
        } else if(base == 16 && ((*p | 32) >= 'a' && (*p | 32) <= 'f')){
            d = (*p | 32) - 'a' + 10;
        } else if(*p == '_' && digits){
            p++;
            continue;
        } else {
            break;
        }
        if(d >= base || v > (long long)(0x3fffffffffffffffLL / base)){
            return fail(c, "intreg invalid");
        }
        v = v * base + d;
        digits++;
        p++;
    }
    if(!digits){
        return fail(c, "valoare neasteptata");
    }
    c->p = p;
    *out = neg ? -v : v;
    return 0;
}

static int is_key_char(char ch){
    return isalnum((unsigned char)ch) || ch == '_' || ch == '-';
}

/* cheie bare sau intre ghilimele */
static int parse_key(Cur *c, char **out){
    if(!at_end(c) && (*c->p == '"' || *c->p == '\'')){
        return parse_string(c, out);
    }
    const char *s = c->p;
    while(!at_end(c) && is_key_char(*c->p)){
        c->p++;
    }
    if(c->p == s){
        return fail(c, "cheie asteptata");
    }
    *out = strndup(s, (size_t)(c->p - s));
    return *out ? 0 : fail(c, "memorie insuficienta");
}

static void strv_free(char **v, int n){
    for(int i = 0; i < n; i++){
        free(v[i]);
    }
    free(v);
}

/* [ "a", "b", ... ] (poate continua pe mai multe linii) */
static int parse_str_array(Cur *c, char ***out, int *nout){
    if(at_end(c) || *c->p != '['){
        return fail(c, "lista asteptata ([...])");
    }
    c->p++;
    char **v = NULL;
    int n = 0, cap = 0;
    for(;;){
        skip_blank(c);
        if(at_end(c)){
            strv_free(v, n);
            return fail(c, "lista neinchisa");
        }
        if(*c->p == ']'){
            c->p++;
            break;
        }
        if(*c->p != '"' && *c->p != '\''){
            strv_free(v, n);
            return fail(c, "in lista sunt permise doar siruri");
        }
        char *s;
        if(parse_string(c, &s) < 0){
            strv_free(v, n);
            return -1;
        }
        if(n == cap){
            cap = cap ? cap * 2 : 4;
            char **t = realloc(v, (size_t)cap * sizeof(*v));
            if(!t){
                free(s);
                strv_free(v, n);
                return fail(c, "memorie insuficienta");
            }
            v = t;
        }
        v[n++] = s;
        skip_blank(c);
        if(!at_end(c) && *c->p == ','){
            c->p++;
        } else if(at_end(c) || *c->p != ']'){
            strv_free(v, n);
            return fail(c, "',' sau ']' asteptat");
        }
    }
    *out = v;
    *nout = n;
    return 0;
}

/* valoare oarecare, ignorata (chei necunoscute: manifestele noi pot adauga campuri) */
static int skip_value(Cur *c, int depth){
    if(depth > 8){
        return fail(c, "imbricare prea adanca");
    }
    if(at_end(c)){
        return fail(c, "valoare asteptata");
    }
    if(*c->p == '"' || *c->p == '\''){
        char *s;
        if(parse_string(c, &s) < 0){
            return -1;
        }
        free(s);
        return 0;
    }
    if(*c->p == '[' || *c->p == '{'){
        char close = *c->p == '[' ? ']' : '}';
        c->p++;
        for(;;){
            skip_blank(c);
            if(at_end(c)){
                return fail(c, "valoare neinchisa");
            }
            if(*c->p == close){
                c->p++;
                return 0;
            }
            if(*c->p == ','){
                c->p++;
                continue;
            }
            if(close == '}'){
                char *k;
                if(parse_key(c, &k) < 0){
                    return -1;
                }
                free(k);
                skip_ws(c);
                if(at_end(c) || *c->p != '='){
                    return fail(c, "'=' asteptat");
                }
                c->p++;
                skip_ws(c);
            }
            if(skip_value(c, depth + 1) < 0){
                return -1;
            }
        }
    }
    long long v;
    if(parse_int(c, &v) == 0){
        return 0;
    }
    /* true/false/data/float: sare peste pana la delimitator */
    const char *s = c->p;
    while(!at_end(c) && *c->p != ',' && *c->p != '\n' && *c->p != '#' && *c->p != ']' && *c->p != '}' && *c->p != ' '){
        c->p++;
    }
    if(c->p == s){
        return fail(c, "valoare neasteptata");
    }
    c->err[0] = 0;
    return 0;
}

static int hex64(const char *s){
    if(strlen(s) != 64){
        return 0;
    }
    for(int i = 0; i < 64; i++){
        if(!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))){
            return 0;
        }
    }
    return 1;
}

/* { sha256 = "...", size = N, mode = 0o755 } */
static int parse_file_entry(Cur *c, PkgFile *f){
    if(at_end(c) || *c->p != '{'){
        return fail(c, "tabel inline asteptat ({ sha256 = ..., size = ... })");
    }
    c->p++;
    int have_sha = 0, have_size = 0;
    f->mode = 0644;
    for(;;){
        skip_ws(c);
        if(at_end(c)){
            return fail(c, "tabel neinchis");
        }
        if(*c->p == '}'){
            c->p++;
            break;
        }
        if(*c->p == ','){
            c->p++;
            continue;
        }
        char *k;
        if(parse_key(c, &k) < 0){
            return -1;
        }
        skip_ws(c);
        if(at_end(c) || *c->p != '='){
            free(k);
            return fail(c, "'=' asteptat");
        }
        c->p++;
        skip_ws(c);
        int rc = 0;
        if(strcmp(k, "sha256") == 0){
            char *s;
            rc = !at_end(c) && (*c->p == '"' || *c->p == '\'') ? parse_string(c, &s) : fail(c, "sir asteptat la sha256");
            if(rc == 0){
                if(!hex64(s)){
                    rc = fail(c, "sha256 trebuie sa aiba 64 de cifre hex minuscule");
                } else {
                    memcpy(f->sha256, s, 65);
                    have_sha = 1;
                }
                free(s);
            }
        } else if(strcmp(k, "size") == 0 || strcmp(k, "mode") == 0){
            long long v;
            rc = parse_int(c, &v);
            if(rc == 0){
                if(k[0] == 's'){
                    f->size = v;
                    have_size = v >= 0;
                    if(v < 0){
                        rc = fail(c, "size negativ");
                    }
                } else {
                    f->mode = (unsigned)v & 0777;
                    if(v < 0 || v > 0777){
                        rc = fail(c, "mode in afara intervalului 0..0o777");
                    }
                }
            }
        } else {
            rc = skip_value(c, 1);
        }
        free(k);
        if(rc < 0){
            return -1;
        }
    }
    if(!have_sha || !have_size){
        return fail(c, "fiecare fisier cere sha256 si size");
    }
    return 0;
}

static int end_of_value(Cur *c){
    skip_ws(c);
    skip_comment(c);
    if(!at_end(c) && *c->p != '\n'){
        return fail(c, "text neasteptat dupa valoare");
    }
    return 0;
}

static int set_str(Cur *c, char **dst, const char *key){
    if(*dst){
        char msg[96];
        snprintf(msg, sizeof(msg), "cheie repetata: %s", key);
        return fail(c, msg);
    }
    if(at_end(c) || (*c->p != '"' && *c->p != '\'')){
        char msg[96];
        snprintf(msg, sizeof(msg), "%s trebuie sa fie un sir", key);
        return fail(c, msg);
    }
    return parse_string(c, dst);
}

static int file_cmp(const void *a, const void *b){
    return strcmp(((const PkgFile *)a)->path, ((const PkgFile *)b)->path);
}

int manifest_parse(const char *text, size_t len, Manifest *m, char *err, size_t errsz){
    memset(m, 0, sizeof(*m));
    m->format = -1;
    Cur cur = {text, text + len, 1, err, errsz};
    Cur *c = &cur;
    int in_files = 0, fcap = 0;
    err[0] = 0;
    for(;;){
        skip_blank(c);
        if(at_end(c)){
            break;
        }
        if(*c->p == '['){
            c->p++;
            if(!at_end(c) && *c->p == '['){
                manifest_free(m);
                return fail(c, "tablourile de tabele ([[...]]) nu sunt suportate");
            }
            const char *s = c->p;
            while(!at_end(c) && *c->p != ']' && *c->p != '\n'){
                c->p++;
            }
            if(at_end(c) || *c->p != ']'){
                manifest_free(m);
                return fail(c, "antet de sectiune neinchis");
            }
            size_t n = (size_t)(c->p - s);
            c->p++;
            if(n != 5 || memcmp(s, "files", 5) != 0){
                manifest_free(m);
                return fail(c, "singura sectiune permisa este [files]");
            }
            in_files = 1;
            if(end_of_value(c) < 0){
                manifest_free(m);
                return -1;
            }
            continue;
        }
        char *key;
        if(parse_key(c, &key) < 0){
            manifest_free(m);
            return -1;
        }
        skip_ws(c);
        if(at_end(c) || *c->p != '='){
            free(key);
            manifest_free(m);
            return fail(c, "'=' asteptat");
        }
        c->p++;
        skip_ws(c);
        int rc = 0;
        if(in_files){
            if(m->nfiles == fcap){
                fcap = fcap ? fcap * 2 : 8;
                PkgFile *t = realloc(m->files, (size_t)fcap * sizeof(*t));
                if(!t){
                    free(key);
                    manifest_free(m);
                    return fail(c, "memorie insuficienta");
                }
                m->files = t;
            }
            PkgFile *f = &m->files[m->nfiles];
            memset(f, 0, sizeof(*f));
            rc = parse_file_entry(c, f);
            if(rc == 0){
                f->path = key;
                key = NULL;
                m->nfiles++;
            }
        } else if(strcmp(key, "format") == 0){
            long long v;
            rc = parse_int(c, &v);
            m->format = (long)v;
        } else if(strcmp(key, "name") == 0){
            rc = set_str(c, &m->name, key);
        } else if(strcmp(key, "version") == 0){
            rc = set_str(c, &m->version, key);
        } else if(strcmp(key, "description") == 0){
            rc = set_str(c, &m->description, key);
        } else if(strcmp(key, "entry") == 0){
            rc = set_str(c, &m->entry, key);
        } else if(strcmp(key, "signature") == 0){
            rc = set_str(c, &m->signature, key);
        } else if(strcmp(key, "deps") == 0 || strcmp(key, "permissions") == 0){
            int deps = key[0] == 'd';
            char ***dst = deps ? &m->deps : &m->perms;
            int *ndst = deps ? &m->ndeps : &m->nperms;
            if(*dst){
                rc = fail(c, "cheie repetata");
            } else {
                rc = parse_str_array(c, dst, ndst);
            }
        } else {
            rc = skip_value(c, 0);
        }
        free(key);
        if(rc == 0){
            rc = end_of_value(c);
        }
        if(rc < 0){
            manifest_free(m);
            return -1;
        }
    }
    qsort(m->files, (size_t)m->nfiles, sizeof(PkgFile), file_cmp);
    for(int i = 1; i < m->nfiles; i++){
        if(strcmp(m->files[i - 1].path, m->files[i].path) == 0){
            snprintf(err, errsz, "manifest.toml: fisier repetat in [files]: %s", m->files[i].path);
            manifest_free(m);
            return -1;
        }
    }
    if(m->format != 1){
        snprintf(err, errsz, "manifest.toml: format = 1 lipseste sau nu e suportat");
        manifest_free(m);
        return -1;
    }
    if(!m->name || !valid_name(m->name)){
        snprintf(err, errsz, "manifest.toml: name lipseste sau e invalid (a-z 0-9 . _ + -, max 64)");
        manifest_free(m);
        return -1;
    }
    if(!m->version || !valid_version(m->version)){
        snprintf(err, errsz, "manifest.toml: version lipseste sau e invalid");
        manifest_free(m);
        return -1;
    }
    if(!m->signature){
        snprintf(err, errsz, "manifest.toml: campul signature lipseste (poate fi gol)");
        manifest_free(m);
        return -1;
    }
    return 0;
}

void manifest_free(Manifest *m){
    free(m->name);
    free(m->version);
    free(m->description);
    free(m->entry);
    free(m->signature);
    strv_free(m->deps, m->ndeps);
    strv_free(m->perms, m->nperms);
    for(int i = 0; i < m->nfiles; i++){
        free(m->files[i].path);
    }
    free(m->files);
    memset(m, 0, sizeof(*m));
}

const PkgFile *manifest_find(const Manifest *m, const char *path){
    PkgFile key = {(char *)path, {0}, 0, 0};
    return bsearch(&key, m->files, (size_t)m->nfiles, sizeof(PkgFile), file_cmp);
}

/* ---- nume si versiuni ---- */

int valid_name(const char *s){
    size_t n = strlen(s);
    if(n == 0 || n > 64 || !(islower((unsigned char)s[0]) || isdigit((unsigned char)s[0]))){
        return 0;
    }
    for(size_t i = 0; i < n; i++){
        char ch = s[i];
        if(!(islower((unsigned char)ch) || isdigit((unsigned char)ch) || ch == '.' || ch == '_' || ch == '+' || ch == '-')){
            return 0;
        }
    }
    return 1;
}

int valid_version(const char *s){
    size_t n = strlen(s);
    if(n == 0 || n > 64 || !isalnum((unsigned char)s[0])){
        return 0;
    }
    for(size_t i = 0; i < n; i++){
        if(!(isalnum((unsigned char)s[i]) || s[i] == '.' || s[i] == '+' || s[i] == '~' || s[i] == '-')){
            return 0;
        }
    }
    return 1;
}

/* Componente separate prin . + ~ -: numerice (comparate ca numere) sau text (strcmp); lipsa = 0 */
int version_cmp(const char *a, const char *b){
    while(*a || *b){
        char ca[32] = "0", cb[32] = "0";
        size_t i = 0;
        while(*a && *a != '.' && *a != '+' && *a != '~' && *a != '-'){
            if(i < sizeof(ca) - 1){
                ca[i++] = *a;
                ca[i] = 0;
            }
            a++;
        }
        i = 0;
        while(*b && *b != '.' && *b != '+' && *b != '~' && *b != '-'){
            if(i < sizeof(cb) - 1){
                cb[i++] = *b;
                cb[i] = 0;
            }
            b++;
        }
        int na = 1, nb = 1;
        for(const char *q = ca; *q; q++){
            na &= isdigit((unsigned char)*q) != 0;
        }
        for(const char *q = cb; *q; q++){
            nb &= isdigit((unsigned char)*q) != 0;
        }
        int d;
        if(na && nb){
            while(ca[0] == '0' && ca[1]){
                memmove(ca, ca + 1, strlen(ca));
            }
            while(cb[0] == '0' && cb[1]){
                memmove(cb, cb + 1, strlen(cb));
            }
            size_t la = strlen(ca), lb = strlen(cb);
            d = la != lb ? (la < lb ? -1 : 1) : strcmp(ca, cb);
        } else {
            d = strcmp(ca, cb);
        }
        if(d){
            return d < 0 ? -1 : 1;
        }
        if(*a){
            a++;
        }
        if(*b){
            b++;
        }
    }
    return 0;
}

int version_ok(const char *have, const char *op, const char *want){
    if(!*op){
        return 1;
    }
    int d = version_cmp(have, want);
    if(strcmp(op, ">=") == 0){
        return d >= 0;
    }
    if(strcmp(op, "<=") == 0){
        return d <= 0;
    }
    if(strcmp(op, ">") == 0){
        return d > 0;
    }
    if(strcmp(op, "<") == 0){
        return d < 0;
    }
    return d == 0;   /* "=" si "==" */
}

int dep_split(const char *spec, char *name, size_t nsz, char *op, char *ver, size_t vsz){
    while(*spec == ' '){
        spec++;
    }
    size_t n = 0;
    while(spec[n] && spec[n] != ' ' && spec[n] != '<' && spec[n] != '>' && spec[n] != '='){
        n++;
    }
    if(n == 0 || n >= nsz){
        return -1;
    }
    memcpy(name, spec, n);
    name[n] = 0;
    spec += n;
    op[0] = 0;
    ver[0] = 0;
    while(*spec == ' '){
        spec++;
    }
    if(!*spec){
        return valid_name(name) ? 0 : -1;
    }
    size_t k = 0;
    while((*spec == '<' || *spec == '>' || *spec == '=') && k < 2){
        op[k++] = *spec++;
    }
    op[k] = 0;
    if(strcmp(op, ">=") && strcmp(op, "<=") && strcmp(op, ">") && strcmp(op, "<") && strcmp(op, "=") && strcmp(op, "==")){
        return -1;
    }
    while(*spec == ' '){
        spec++;
    }
    size_t vl = strlen(spec);
    while(vl && spec[vl - 1] == ' '){
        vl--;
    }
    if(vl == 0 || vl >= vsz){
        return -1;
    }
    memcpy(ver, spec, vl);
    ver[vl] = 0;
    return valid_name(name) && valid_version(ver) ? 0 : -1;
}

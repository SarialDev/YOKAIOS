/* stdio: FILE* cu buffer (un singur buffer pentru citire sau scriere), familia printf (cu %e %f %g %a prin fpconv.c),
 * sscanf simplu. Fara thread-uri: fara lock-uri. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <math.h>

char *__fp_format(double v, char conv, int prec, int alt, int *len);
char *__fp_format_hex(double v, int prec, int upper, int alt, int *len);

#define F_READ 1
#define F_WRITE 2
#define F_EOF 4
#define F_ERR 8
#define F_OWNBUF 16
#define F_APPEND 32
#define F_STATIC 64

struct _FILE {
    int fd;
    int flags;
    int bufmode;                // _IOFBF/_IOLBF/_IONBF
    char *buf;
    size_t size;
    size_t rpos, rend;          // citire: buf[rpos..rend)
    size_t wpos;                // scriere: buf[0..wpos)
    int pushback;               // ungetc, -1 daca nu exista
    struct _FILE *next;
};

static char stdin_buf[BUFSIZ];
static char stdout_buf[BUFSIZ];
static FILE f_stdin = {0, F_READ | F_STATIC, _IOFBF, stdin_buf, BUFSIZ, 0, 0, 0, -1, NULL};
static FILE f_stdout = {1, F_WRITE | F_STATIC, _IOLBF, stdout_buf, BUFSIZ, 0, 0, 0, -1, NULL};
static FILE f_stderr = {2, F_WRITE | F_STATIC, _IONBF, NULL, 0, 0, 0, 0, -1, NULL};
FILE *stdin = &f_stdin;
FILE *stdout = &f_stdout;
FILE *stderr = &f_stderr;
static FILE *open_files = &f_stdin;

static void init_std(void){
    static int done;
    if(done){
        return;
    }
    done = 1;
    f_stdin.next = &f_stdout;
    f_stdout.next = &f_stderr;
    f_stdout.bufmode = isatty(1) ? _IOLBF : _IOFBF;
}

static int flush_write(FILE *f){
    size_t off = 0;
    while(off < f->wpos){
        ssize_t n = write(f->fd, f->buf + off, f->wpos - off);
        if(n < 0){
            f->flags |= F_ERR;
            f->wpos = 0;
            return -1;
        }
        off += (size_t)n;
    }
    f->wpos = 0;
    return 0;
}

// Pregateste fisierul pentru citire (arunca bufferul de scriere) sau scriere (aduce pozitia OS inapoi)
static int to_read(FILE *f){
    init_std();
    if(!(f->flags & F_READ)){
        errno = EBADF;
        f->flags |= F_ERR;
        return -1;
    }
    if(f->wpos && flush_write(f) < 0){
        return -1;
    }
    return 0;
}

static int to_write(FILE *f){
    init_std();
    if(!(f->flags & F_WRITE)){
        errno = EBADF;
        f->flags |= F_ERR;
        return -1;
    }
    if(f->rend > f->rpos){
        lseek(f->fd, -(long)(f->rend - f->rpos), SEEK_CUR);
    }
    f->rpos = f->rend = 0;
    f->pushback = -1;
    if(!f->buf && f->bufmode != _IONBF){
        f->buf = malloc(BUFSIZ);
        if(f->buf){
            f->size = BUFSIZ;
            f->flags |= F_OWNBUF;
        } else {
            f->bufmode = _IONBF;
        }
    }
    return 0;
}

int fflush(FILE *f){
    if(!f){
        int r = 0;
        init_std();
        for(FILE *p = open_files; p; p = p->next){
            if((p->flags & F_WRITE) && fflush(p) < 0){
                r = -1;
            }
        }
        return r;
    }
    if(f->wpos){
        return flush_write(f);
    }
    return 0;
}

int fputc(int c, FILE *f){
    if(to_write(f) < 0){
        return EOF;
    }
    unsigned char ch = (unsigned char)c;
    if(f->bufmode == _IONBF || !f->buf){
        if(write(f->fd, &ch, 1) != 1){
            f->flags |= F_ERR;
            return EOF;
        }
        return ch;
    }
    if(f->wpos == f->size && flush_write(f) < 0){
        return EOF;
    }
    f->buf[f->wpos++] = (char)ch;
    if((f->bufmode == _IOLBF && ch == '\n') && flush_write(f) < 0){
        return EOF;
    }
    return ch;
}

int putc(int c, FILE *f){ return fputc(c, f); }
int putchar(int c){ return fputc(c, stdout); }

size_t fwrite(const void *p, size_t size, size_t n, FILE *f){
    size_t total = size * n;
    const unsigned char *s = p;
    if(!total){
        return 0;
    }
    if(to_write(f) < 0){
        return 0;
    }
    if(f->bufmode == _IONBF || !f->buf){
        size_t off = 0;
        while(off < total){
            ssize_t w = write(f->fd, s + off, total - off);
            if(w < 0){
                f->flags |= F_ERR;
                break;
            }
            off += (size_t)w;
        }
        return off / size;
    }
    for(size_t i = 0; i < total; i++){
        if(fputc(s[i], f) == EOF){
            return i / size;
        }
    }
    return n;
}

int fputs(const char *s, FILE *f){
    size_t l = strlen(s);
    return fwrite(s, 1, l, f) == l ? 0 : EOF;
}

int puts(const char *s){
    if(fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF){
        return EOF;
    }
    return 0;
}

static int fill(FILE *f){
    if(!f->buf){
        f->buf = malloc(BUFSIZ);
        if(!f->buf){
            f->flags |= F_ERR;
            return -1;
        }
        f->size = BUFSIZ;
        f->flags |= F_OWNBUF;
    }
    // Citirea de pe stdin goleste intai stdout (ca terminalele)
    if(f == stdin){
        fflush(stdout);
    }
    ssize_t n = read(f->fd, f->buf, f->size);
    if(n < 0){
        f->flags |= F_ERR;
        return -1;
    }
    if(n == 0){
        f->flags |= F_EOF;
        return -1;
    }
    f->rpos = 0;
    f->rend = (size_t)n;
    return 0;
}

int fgetc(FILE *f){
    if(to_read(f) < 0){
        return EOF;
    }
    if(f->pushback >= 0){
        int c = f->pushback;
        f->pushback = -1;
        return c;
    }
    if(f->rpos == f->rend && fill(f) < 0){
        return EOF;
    }
    return (unsigned char)f->buf[f->rpos++];
}

int getc(FILE *f){ return fgetc(f); }
int getchar(void){ return fgetc(stdin); }

int ungetc(int c, FILE *f){
    if(c == EOF || f->pushback >= 0){
        return EOF;
    }
    f->pushback = (unsigned char)c;
    f->flags &= ~F_EOF;
    return c;
}

size_t fread(void *p, size_t size, size_t n, FILE *f){
    size_t total = size * n;
    unsigned char *d = p;
    size_t got = 0;
    if(!total){
        return 0;
    }
    if(to_read(f) < 0){
        return 0;
    }
    while(got < total){
        if(f->pushback >= 0){
            d[got++] = (unsigned char)f->pushback;
            f->pushback = -1;
            continue;
        }
        if(f->rpos == f->rend){
            if(total - got >= f->size && f->buf){
                // Cerere mare: direct in destinatie
                ssize_t r = read(f->fd, d + got, total - got);
                if(r <= 0){
                    f->flags |= r < 0 ? F_ERR : F_EOF;
                    break;
                }
                got += (size_t)r;
                continue;
            }
            if(fill(f) < 0){
                break;
            }
        }
        size_t c = f->rend - f->rpos;
        if(c > total - got){
            c = total - got;
        }
        memcpy(d + got, f->buf + f->rpos, c);
        f->rpos += c;
        got += c;
    }
    return got / size;
}

char *fgets(char *s, int n, FILE *f){
    int i = 0;
    if(n <= 0){
        return NULL;
    }
    while(i < n - 1){
        int c = fgetc(f);
        if(c == EOF){
            break;
        }
        s[i++] = (char)c;
        if(c == '\n'){
            break;
        }
    }
    if(i == 0){
        return NULL;
    }
    s[i] = 0;
    return s;
}

long getline(char **line, size_t *cap, FILE *f){
    size_t len = 0;
    if(!*line || !*cap){
        *cap = 128;
        *line = malloc(*cap);
        if(!*line){
            return -1;
        }
    }
    int c;
    while((c = fgetc(f)) != EOF){
        if(len + 2 > *cap){
            char *n = realloc(*line, *cap * 2);
            if(!n){
                return -1;
            }
            *line = n;
            *cap *= 2;
        }
        (*line)[len++] = (char)c;
        if(c == '\n'){
            break;
        }
    }
    if(len == 0){
        return -1;
    }
    (*line)[len] = 0;
    return (long)len;
}

static int parse_mode(const char *mode, int *oflags, int *fflags){
    int plus = strchr(mode, '+') != NULL;
    switch(mode[0]){
        case 'r':
            *oflags = plus ? O_RDWR : O_RDONLY;
            *fflags = F_READ | (plus ? F_WRITE : 0);
            return 0;
        case 'w':
            *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
            *fflags = F_WRITE | (plus ? F_READ : 0);
            return 0;
        case 'a':
            *oflags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
            *fflags = F_WRITE | F_APPEND | (plus ? F_READ : 0);
            return 0;
        default:
            errno = EINVAL;
            return -1;
    }
}

FILE *fdopen(int fd, const char *mode){
    int oflags, fflags;
    if(parse_mode(mode, &oflags, &fflags) < 0){
        return NULL;
    }
    init_std();
    FILE *f = calloc(1, sizeof(FILE));
    if(!f){
        return NULL;
    }
    f->fd = fd;
    f->flags = fflags;
    f->bufmode = _IOFBF;
    f->pushback = -1;
    f->next = open_files;
    open_files = f;
    return f;
}

FILE *fopen(const char *path, const char *mode){
    int oflags, fflags;
    if(parse_mode(mode, &oflags, &fflags) < 0){
        return NULL;
    }
    int fd = open(path, oflags, 0666);
    if(fd < 0){
        return NULL;
    }
    FILE *f = fdopen(fd, mode);
    if(!f){
        close(fd);
    }
    return f;
}

int fclose(FILE *f){
    int r = 0;
    init_std();
    if(f->flags & F_WRITE){
        r = fflush(f);
    }
    if(close(f->fd) < 0){
        r = -1;
    }
    for(FILE **pp = &open_files; *pp; pp = &(*pp)->next){
        if(*pp == f){
            *pp = f->next;
            break;
        }
    }
    if(f->flags & F_OWNBUF){
        free(f->buf);
    }
    if(!(f->flags & F_STATIC)){
        free(f);
    }
    return r;
}

int fseek(FILE *f, long off, int whence){
    if(fflush(f) < 0){
        return -1;
    }
    if(whence == SEEK_CUR){
        off -= (long)(f->rend - f->rpos) + (f->pushback >= 0 ? 1 : 0);
    }
    f->rpos = f->rend = 0;
    f->pushback = -1;
    f->flags &= ~F_EOF;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f){
    long pos = lseek(f->fd, 0, SEEK_CUR);
    if(pos < 0){
        return -1;
    }
    return pos - (long)(f->rend - f->rpos) - (f->pushback >= 0 ? 1 : 0) + (long)f->wpos;
}

void rewind(FILE *f){
    fseek(f, 0, SEEK_SET);
    f->flags &= ~F_ERR;
}

int feof(FILE *f){ return (f->flags & F_EOF) != 0; }
int ferror(FILE *f){ return (f->flags & F_ERR) != 0; }
void clearerr(FILE *f){ f->flags &= ~(F_EOF | F_ERR); }
int fileno(FILE *f){ return f->fd; }

int setvbuf(FILE *f, char *buf, int mode, size_t size){
    if(f->wpos){
        flush_write(f);
    }
    if(f->flags & F_OWNBUF){
        free(f->buf);
        f->flags &= ~F_OWNBUF;
    }
    f->rpos = f->rend = 0;
    f->bufmode = mode;
    f->buf = mode == _IONBF ? NULL : buf;
    f->size = f->buf ? size : 0;
    return 0;
}

void setbuf(FILE *f, char *buf){
    setvbuf(f, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

void perror(const char *s){
    if(s && *s){
        fprintf(stderr, "%s: ", s);
    }
    fprintf(stderr, "%s\n", strerror(errno));
}

/* ---------- printf ---------- */

typedef struct {
    void (*put)(void *ctx, char c);
    void *ctx;
    long count;
} Out;

static void out_char(Out *o, char c){
    o->put(o->ctx, c);
    o->count++;
}

static void out_pad(Out *o, char c, int n){
    while(n-- > 0){
        out_char(o, c);
    }
}

static int format(Out *o, const char *fmt, va_list ap){
    for(; *fmt; fmt++){
        if(*fmt != '%'){
            out_char(o, *fmt);
            continue;
        }
        fmt++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0;
        for(;; fmt++){
            if(*fmt == '-') left = 1;
            else if(*fmt == '+') plus = 1;
            else if(*fmt == ' ') space = 1;
            else if(*fmt == '#') alt = 1;
            else if(*fmt == '0') zero = 1;
            else break;
        }
        int width = 0, prec = -1;
        if(*fmt == '*'){
            width = va_arg(ap, int);
            if(width < 0){
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while(isdigit((unsigned char)*fmt)){
                width = width * 10 + (*fmt++ - '0');
            }
        }
        if(*fmt == '.'){
            fmt++;
            prec = 0;
            if(*fmt == '*'){
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while(isdigit((unsigned char)*fmt)){
                    prec = prec * 10 + (*fmt++ - '0');
                }
            }
        }
        int lng = 0;           // 0 int, 1 long, 2 long long, -1 short, -2 char; size_t/intmax_t/ptrdiff_t = long
        for(;; fmt++){
            if(*fmt == 'l') lng = lng >= 1 ? 2 : 1;
            else if(*fmt == 'h') lng = lng <= -1 ? -2 : -1;
            else if(*fmt == 'z' || *fmt == 'j' || *fmt == 't') lng = 1;
            else break;
        }
        char tmp[24];
        char *fbuf = NULL;                      // corpul unui numar in virgula mobila (malloc)
        int is_float = 0, finite = 1;
        const char *body = tmp;
        int blen = 0;
        const char *prefix = "";
        int plen = 0;
        char conv = *fmt;
        switch(conv){
            case 'd': case 'i': {
                long long v = lng >= 1 ? (lng == 2 ? va_arg(ap, long long) : va_arg(ap, long))
                                       : (lng == -1 ? (short)va_arg(ap, int)
                                                    : lng == -2 ? (signed char)va_arg(ap, int) : va_arg(ap, int));
                unsigned long long u = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
                prefix = v < 0 ? "-" : plus ? "+" : space ? " " : "";
                plen = *prefix ? 1 : 0;
                char *e = tmp + sizeof(tmp);
                char *p = e;
                while(u){
                    *--p = (char)('0' + u % 10);
                    u /= 10;
                }
                body = p;
                blen = (int)(e - p);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': case 'p': {
                unsigned long long u;
                if(conv == 'p'){
                    u = (uintptr_t)va_arg(ap, void *);
                    alt = 1;
                    conv = 'x';
                } else {
                    u = lng >= 1 ? (lng == 2 ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned long))
                                 : (lng == -1 ? (unsigned short)va_arg(ap, unsigned)
                                              : lng == -2 ? (unsigned char)va_arg(ap, unsigned) : va_arg(ap, unsigned));
                }
                unsigned base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
                const char *digits = conv == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
                char *e = tmp + sizeof(tmp);
                char *p = e;
                unsigned long long orig = u;
                while(u){
                    *--p = digits[u % base];
                    u /= base;
                }
                body = p;
                blen = (int)(e - p);
                if(alt && base == 16 && (orig || *fmt == 'p')){
                    prefix = conv == 'X' ? "0X" : "0x";
                    plen = 2;
                } else if(alt && base == 8 && (blen == 0 || *p != '0')){
                    prefix = "0";
                    plen = 1;
                }
                break;
            }
            case 'c':
                tmp[0] = (char)va_arg(ap, int);
                blen = 1;
                prec = -1;
                break;
            case 's': {
                body = va_arg(ap, const char *);
                if(!body){
                    body = "(null)";
                }
                blen = 0;
                while(body[blen] && (prec < 0 || blen < prec)){
                    blen++;
                }
                break;
            }
            case '%':
                tmp[0] = '%';
                blen = 1;
                break;
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                double v = va_arg(ap, double);
                int upper = conv == 'F' || conv == 'E' || conv == 'G' || conv == 'A';
                is_float = 1;
                prefix = signbit(v) ? "-" : plus ? "+" : space ? " " : "";
                plen = *prefix ? 1 : 0;
                if(isnan(v) || isinf(v)){
                    finite = 0;
                    body = isnan(v) ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
                    if(isnan(v)){
                        prefix = signbit(v) ? "-" : plus ? "+" : space ? " " : "";
                        plen = *prefix ? 1 : 0;
                    }
                    blen = 3;
                    break;
                }
                double av = fabs(v);
                if(conv == 'a' || conv == 'A'){
                    fbuf = __fp_format_hex(av, prec, upper, alt, &blen);
                } else {
                    fbuf = __fp_format(av, conv, prec < 0 ? 6 : prec, alt, &blen);
                }
                if(!fbuf){
                    body = "";
                    blen = 0;
                } else {
                    body = fbuf;
                }
                break;
            }
            case 0:
                return (int)o->count;
            default:
                out_char(o, '%');
                out_char(o, conv);
                continue;
        }
        int is_int = conv == 'd' || conv == 'i' || conv == 'u' || conv == 'x' || conv == 'X' || conv == 'o';
        int zeros = 0;
        if(is_int){
            if(prec >= 0){
                zero = 0;
                if(prec > blen){
                    zeros = prec - blen;
                }
                if(prec == 0 && blen == 0 && !(alt && conv == 'o')){
                    plen = 0;
                }
            } else if(blen == 0){
                zeros = 1;                  // valoarea 0
            }
        }
        int total = plen + zeros + blen;
        if(((is_int && prec < 0) || (is_float && finite)) && zero && !left && width > total){
            zeros += width - total;
            total = width;
        }
        if(is_float && fbuf && (conv == 'a' || conv == 'A') && zeros){
            // zerourile de umplere vin dupa "0x"
            for(int i = 0; i < plen; i++){
                out_char(o, prefix[i]);
            }
            plen = 0;
            out_char(o, body[0]);
            out_char(o, body[1]);
            body += 2;
            blen -= 2;
            total -= 2;
        }
        if(!left){
            out_pad(o, ' ', width - total);
        }
        for(int i = 0; i < plen; i++){
            out_char(o, prefix[i]);
        }
        out_pad(o, '0', zeros);
        for(int i = 0; i < blen; i++){
            out_char(o, body[i]);
        }
        if(left){
            out_pad(o, ' ', width - total);
        }
        free(fbuf);
    }
    return (int)o->count;
}

static void put_file(void *ctx, char c){ fputc(c, ctx); }

int vfprintf(FILE *f, const char *fmt, va_list ap){
    Out o = {put_file, f, 0};
    return format(&o, fmt, ap);
}

int vprintf(const char *fmt, va_list ap){ return vfprintf(stdout, fmt, ap); }

int fprintf(FILE *f, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(f, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return r;
}

typedef struct {
    char *s;
    size_t cap;         // locul ramas pentru caractere (fara terminator)
} StrCtx;

static void put_str(void *ctx, char c){
    StrCtx *sc = ctx;
    if(sc->cap){
        *sc->s++ = c;
        sc->cap--;
    }
}

int vsnprintf(char *s, size_t n, const char *fmt, va_list ap){
    StrCtx sc = {s, n ? n - 1 : 0};
    Out o = {put_str, &sc, 0};
    int r = format(&o, fmt, ap);
    if(n){
        *sc.s = 0;
    }
    return r;
}

int vsprintf(char *s, const char *fmt, va_list ap){
    return vsnprintf(s, (size_t)INT_MAX, fmt, ap);
}

int snprintf(char *s, size_t n, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, n, fmt, ap);
    va_end(ap);
    return r;
}

int sprintf(char *s, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(s, (size_t)INT_MAX, fmt, ap);
    va_end(ap);
    return r;
}

static void put_fd(void *ctx, char c){ (void)write(*(int *)ctx, &c, 1); }

int dprintf(int fd, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    Out o = {put_fd, &fd, 0};
    int r = format(&o, fmt, ap);
    va_end(ap);
    return r;
}

/* ---------- sscanf: %d %i %u %x %o %ld %lld %s %c %% %[...] nu; spatii in format = orice spatiu ---------- */

int sscanf(const char *str, const char *fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    const char *s = str;
    int assigned = 0;
    for(; *fmt; fmt++){
        if(isspace((unsigned char)*fmt)){
            while(isspace((unsigned char)*s)){
                s++;
            }
            continue;
        }
        if(*fmt != '%'){
            if(*s != *fmt){
                break;
            }
            s++;
            continue;
        }
        fmt++;
        int suppress = 0;
        if(*fmt == '*'){
            suppress = 1;
            fmt++;
        }
        int width = 0;
        while(isdigit((unsigned char)*fmt)){
            width = width * 10 + (*fmt++ - '0');
        }
        int lng = 0;
        for(;; fmt++){
            if(*fmt == 'l') lng++;
            else if(*fmt == 'h') lng--;
            else if(*fmt == 'z') lng = 1;
            else break;
        }
        char c = *fmt;
        if(c == '%'){
            if(*s != '%'){
                break;
            }
            s++;
            continue;
        }
        if(c != 'c'){
            while(isspace((unsigned char)*s)){
                s++;
            }
        }
        if(!*s){
            if(!assigned){
                assigned = -1;
            }
            break;
        }
        if(c == 'c'){
            if(!suppress){
                *va_arg(ap, char *) = *s;
                assigned++;
            }
            s++;
        } else if(c == 's'){
            char *d = suppress ? NULL : va_arg(ap, char *);
            int n = 0;
            while(*s && !isspace((unsigned char)*s) && (!width || n < width)){
                if(d){
                    d[n] = *s;
                }
                n++;
                s++;
            }
            if(d){
                d[n] = 0;
                assigned++;
            }
        } else if(c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o'){
            char *end;
            int base = c == 'x' || c == 'X' ? 16 : c == 'o' ? 8 : c == 'i' ? 0 : 10;
            long long v = c == 'u' ? (long long)strtoull(s, &end, base) : strtoll(s, &end, base);
            if(end == s){
                break;
            }
            s = end;
            if(!suppress){
                if(lng >= 2) *va_arg(ap, long long *) = v;
                else if(lng == 1) *va_arg(ap, long *) = (long)v;
                else if(lng == -1) *va_arg(ap, short *) = (short)v;
                else if(lng <= -2) *va_arg(ap, signed char *) = (signed char)v;
                else *va_arg(ap, int *) = (int)v;
                assigned++;
            }
        } else {
            break;
        }
    }
    va_end(ap);
    return assigned;
}

/* ---------- extra pentru porturi (Lua): fseeko/ftello, flockfile, tmpfile, freopen, popen ---------- */

int fseeko(FILE *f, long off, int whence){ return fseek(f, off, whence); }
long ftello(FILE *f){ return ftell(f); }
int getc_unlocked(FILE *f){ return fgetc(f); }
int putc_unlocked(int c, FILE *f){ return fputc(c, f); }
void flockfile(FILE *f){ (void)f; }
void funlockfile(FILE *f){ (void)f; }

FILE *freopen(const char *path, const char *mode, FILE *f){
    int oflags, fflags;
    if(!path || parse_mode(mode, &oflags, &fflags) < 0){
        errno = EINVAL;
        return NULL;
    }
    fflush(f);
    int fd = open(path, oflags, 0666);
    if(fd < 0){
        return NULL;
    }
    if(f->fd == fd){
        return f;
    }
    // pastreaza descriptorul numeric al lui f (stdin/stdout/stderr raman 0/1/2)
    if(dup2(fd, f->fd) < 0){
        close(fd);
        return NULL;
    }
    close(fd);
    f->flags = (f->flags & (F_OWNBUF | F_STATIC)) | fflags;
    f->rpos = f->rend = f->wpos = 0;
    f->pushback = -1;
    return f;
}

static unsigned tmp_counter;

char *tmpnam(char *s){
    static char buf[L_tmpnam];
    char *d = s ? s : buf;
    for(int tries = 0; tries < 1000; tries++){
        snprintf(d, L_tmpnam, "/tmp/tmp%d_%u", (int)getpid(), tmp_counter++);
        if(access(d, 0) != 0){
            return d;
        }
    }
    return NULL;
}

int mkstemp(char *tmpl){
    size_t n = strlen(tmpl);
    if(n < 6 || strcmp(tmpl + n - 6, "XXXXXX") != 0){
        errno = EINVAL;
        return -1;
    }
    for(int tries = 0; tries < 1000; tries++){
        unsigned v = (unsigned)getpid() * 2654435761u + tmp_counter++ * 40503u;
        for(int i = 0; i < 6; i++){
            tmpl[n - 6 + i] = "abcdefghijklmnopqrstuvwxyz0123456789"[v % 36];
            v = v / 36 + 12345u * (unsigned)(i + 1);
        }
        int fd = open(tmpl, O_RDWR | O_CREAT | O_EXCL, 0600);
        if(fd >= 0){
            return fd;
        }
        if(errno != EEXIST){
            return -1;
        }
    }
    return -1;
}

FILE *tmpfile(void){
    char name[L_tmpnam];
    if(!tmpnam(name)){
        return NULL;
    }
    int fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if(fd < 0){
        return NULL;
    }
    unlink(name);                       // fisierul dispare la inchidere
    FILE *f = fdopen(fd, "w+");
    if(!f){
        close(fd);
    }
    return f;
}

/* popen/pclose/system: fork + execve("/bin/ysh", "-c", cmd) */
extern char **environ;
static FILE *popen_files[16];
static pid_t popen_pids[16];

static pid_t spawn_sh(const char *cmd, int in_fd, int out_fd){
    pid_t pid = fork();
    if(pid == 0){
        if(in_fd >= 0 && in_fd != 0){
            dup2(in_fd, 0);
            close(in_fd);
        }
        if(out_fd >= 0 && out_fd != 1){
            dup2(out_fd, 1);
            close(out_fd);
        }
        char *const argv[] = {(char *)"ysh", (char *)"-c", (char *)cmd, NULL};
        execve("/bin/ysh", argv, environ);
        _exit(127);
    }
    return pid;
}

FILE *popen(const char *cmd, const char *mode){
    int rd = mode[0] == 'r';
    if(!rd && mode[0] != 'w'){
        errno = EINVAL;
        return NULL;
    }
    int p[2];
    if(pipe(p) < 0){
        return NULL;
    }
    fflush(NULL);
    pid_t pid = rd ? spawn_sh(cmd, -1, p[1]) : spawn_sh(cmd, p[0], -1);
    if(pid < 0){
        close(p[0]);
        close(p[1]);
        return NULL;
    }
    int mine = rd ? p[0] : p[1];
    close(rd ? p[1] : p[0]);
    FILE *f = fdopen(mine, rd ? "r" : "w");
    if(!f){
        close(mine);
        waitpid(pid, NULL, 0);
        return NULL;
    }
    for(int i = 0; i < 16; i++){
        if(!popen_files[i]){
            popen_files[i] = f;
            popen_pids[i] = pid;
            break;
        }
    }
    return f;
}

int pclose(FILE *f){
    pid_t pid = -1;
    for(int i = 0; i < 16; i++){
        if(popen_files[i] == f){
            pid = popen_pids[i];
            popen_files[i] = NULL;
        }
    }
    fclose(f);
    if(pid < 0){
        errno = ECHILD;
        return -1;
    }
    int status = 0;
    if(waitpid(pid, &status, 0) < 0){
        return -1;
    }
    return status;
}

int system(const char *cmd){
    if(!cmd){
        return 1;                       // exista un interpretor de comenzi
    }
    fflush(NULL);
    pid_t pid = spawn_sh(cmd, -1, -1);
    if(pid < 0){
        return -1;
    }
    int status = 0;
    if(waitpid(pid, &status, 0) < 0){
        return -1;
    }
    return status;
}

/* Cititor de arhive tar ustar (fisiere si directoare) */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "ypkg.h"

static int read_full(int fd, void *buf, size_t n){
    size_t got = 0;
    while(got < n){
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if(r < 0){
            return -1;
        }
        if(r == 0){
            break;
        }
        got += (size_t)r;
    }
    return (int)got;
}

static int octal(const unsigned char *p, size_t n, long long *out){
    long long v = 0;
    size_t i = 0;
    while(i < n && (p[i] == ' ' || p[i] == 0)){
        i++;
    }
    int digits = 0;
    for(; i < n && p[i] >= '0' && p[i] <= '7'; i++){
        v = v * 8 + (p[i] - '0');
        digits++;
        if(v > (1LL << 50)){
            return -1;
        }
    }
    for(; i < n; i++){
        if(p[i] != ' ' && p[i] != 0){
            return -1;
        }
    }
    if(!digits){
        return -1;
    }
    *out = v;
    return 0;
}

int tar_next(int fd, TarEnt *e, char *err, size_t errsz){
    unsigned char h[512];
    int got = read_full(fd, h, sizeof(h));
    if(got < 0){
        snprintf(err, errsz, "eroare de citire");
        return -1;
    }
    if(got == 0){
        return 0;
    }
    if(got < 512){
        snprintf(err, errsz, "arhiva trunchiata (antet incomplet)");
        return -1;
    }
    int zero = 1;
    for(int i = 0; i < 512; i++){
        if(h[i]){
            zero = 0;
            break;
        }
    }
    if(zero){
        return 0;
    }
    long long chk;
    if(octal(h + 148, 8, &chk) < 0){
        snprintf(err, errsz, "antet tar invalid (checksum)");
        return -1;
    }
    long long sum = 0;
    for(int i = 0; i < 512; i++){
        sum += (i >= 148 && i < 156) ? ' ' : h[i];
    }
    if(sum != chk){
        snprintf(err, errsz, "antet tar invalid (checksum gresit)");
        return -1;
    }
    if(memcmp(h + 257, "ustar", 5) != 0){
        snprintf(err, errsz, "doar formatul ustar e acceptat");
        return -1;
    }
    long long size, mode;
    if(octal(h + 124, 12, &size) < 0 || octal(h + 100, 8, &mode) < 0){
        snprintf(err, errsz, "antet tar invalid (marime sau mod)");
        return -1;
    }
    char name[101], prefix[156];
    memcpy(name, h, 100);
    name[100] = 0;
    memcpy(prefix, h + 345, 155);
    prefix[155] = 0;
    if(prefix[0]){
        snprintf(e->name, sizeof(e->name), "%s/%s", prefix, name);
    } else {
        snprintf(e->name, sizeof(e->name), "%s", name);
    }
    e->type = h[156] == 0 ? '0' : (char)h[156];
    e->mode = (unsigned)mode & 07777;
    e->size = size;
    return 1;
}

static long long padding(long long size){
    return (512 - size % 512) % 512;
}

int tar_skip_data(int fd, const TarEnt *e){
    long long n = e->size + padding(e->size);
    return n == 0 || lseek(fd, (off_t)n, SEEK_CUR) >= 0 ? 0 : -1;
}

int tar_stream(int fd, const TarEnt *e, Sha256 *sha, int outfd){
    static unsigned char buf[8192];
    long long left = e->size;
    while(left > 0){
        size_t want = left > (long long)sizeof(buf) ? sizeof(buf) : (size_t)left;
        int got = read_full(fd, buf, want);
        if(got != (int)want){
            return -1;
        }
        if(sha){
            sha256_update(sha, buf, want);
        }
        if(outfd >= 0){
            size_t off = 0;
            while(off < want){
                ssize_t w = write(outfd, buf + off, want - off);
                if(w <= 0){
                    return -2;
                }
                off += (size_t)w;
            }
        }
        left -= (long long)want;
    }
    long long pad = padding(e->size);
    if(pad && lseek(fd, (off_t)pad, SEEK_CUR) < 0){
        return -1;
    }
    return 0;
}

/* Elimina "./" de la inceput si '/' de la sfarsit; refuza cai absolute, goale, cu componente goale, "." sau ".." */
int path_normalize(char *p){
    while(p[0] == '.' && p[1] == '/'){
        memmove(p, p + 2, strlen(p + 1));
    }
    size_t n = strlen(p);
    while(n > 1 && p[n - 1] == '/'){
        p[--n] = 0;
    }
    if(n == 0 || n > 400 || p[0] == '/'){
        return -1;
    }
    const char *c = p;
    while(*c){
        const char *e = strchr(c, '/');
        size_t l = e ? (size_t)(e - c) : strlen(c);
        if(l == 0 || (l == 1 && c[0] == '.') || (l == 2 && c[0] == '.' && c[1] == '.')){
            return -1;
        }
        for(size_t i = 0; i < l; i++){
            if((unsigned char)c[i] < 0x20 || c[i] == 0x7f){
                return -1;
            }
        }
        if(!e){
            break;
        }
        c = e + 1;
    }
    return 0;
}

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

void *memcpy(void *d, const void *s, size_t n){
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}

void *memmove(void *d, const void *s, size_t n){
    unsigned char *dp = d;
    const unsigned char *sp = s;
    if(dp == sp || n == 0){
        return d;
    }
    if(dp < sp || dp >= sp + n){
        return memcpy(d, s, n);
    }
    while(n--){
        dp[n] = sp[n];
    }
    return d;
}

void *memset(void *d, int c, size_t n){
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

int memcmp(const void *a, const void *b, size_t n){
    const unsigned char *x = a, *y = b;
    for(size_t i = 0; i < n; i++){
        if(x[i] != y[i]){
            return x[i] - y[i];
        }
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n){
    const unsigned char *p = s;
    for(size_t i = 0; i < n; i++){
        if(p[i] == (unsigned char)c){
            return (void *)(p + i);
        }
    }
    return NULL;
}

void *memrchr(const void *s, int c, size_t n){
    const unsigned char *p = s;
    while(n--){
        if(p[n] == (unsigned char)c){
            return (void *)(p + n);
        }
    }
    return NULL;
}

size_t strlen(const char *s){
    const char *p = s;
    while(*p){
        p++;
    }
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t n){
    size_t i = 0;
    while(i < n && s[i]){
        i++;
    }
    return i;
}

char *strcpy(char *d, const char *s){
    char *r = d;
    while((*d++ = *s++)){
    }
    return r;
}

char *strncpy(char *d, const char *s, size_t n){
    size_t i = 0;
    for(; i < n && s[i]; i++){
        d[i] = s[i];
    }
    for(; i < n; i++){
        d[i] = 0;
    }
    return d;
}

size_t strlcpy(char *d, const char *s, size_t n){
    size_t len = strlen(s);
    if(n){
        size_t c = len < n - 1 ? len : n - 1;
        memcpy(d, s, c);
        d[c] = 0;
    }
    return len;
}

char *strcat(char *d, const char *s){
    strcpy(d + strlen(d), s);
    return d;
}

char *strncat(char *d, const char *s, size_t n){
    char *e = d + strlen(d);
    size_t i = 0;
    for(; i < n && s[i]; i++){
        e[i] = s[i];
    }
    e[i] = 0;
    return d;
}

int strcmp(const char *a, const char *b){
    while(*a && *a == *b){
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n){
    for(size_t i = 0; i < n; i++){
        if(a[i] != b[i] || !a[i]){
            return (unsigned char)a[i] - (unsigned char)b[i];
        }
    }
    return 0;
}

int strcasecmp(const char *a, const char *b){
    while(*a && tolower((unsigned char)*a) == tolower((unsigned char)*b)){
        a++;
        b++;
    }
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n){
    for(size_t i = 0; i < n; i++){
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if(x != y || !x){
            return x - y;
        }
    }
    return 0;
}

char *strchr(const char *s, int c){
    for(;; s++){
        if(*s == (char)c){
            return (char *)s;
        }
        if(!*s){
            return NULL;
        }
    }
}

char *strrchr(const char *s, int c){
    const char *r = NULL;
    for(;; s++){
        if(*s == (char)c){
            r = s;
        }
        if(!*s){
            return (char *)r;
        }
    }
}

char *strstr(const char *h, const char *n){
    size_t nl = strlen(n);
    if(!nl){
        return (char *)h;
    }
    for(; *h; h++){
        if(*h == *n && strncmp(h, n, nl) == 0){
            return (char *)h;
        }
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept){
    size_t i = 0;
    while(s[i] && strchr(accept, s[i])){
        i++;
    }
    return i;
}

size_t strcspn(const char *s, const char *reject){
    size_t i = 0;
    while(s[i] && !strchr(reject, s[i])){
        i++;
    }
    return i;
}

char *strpbrk(const char *s, const char *accept){
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}

char *strsep(char **sp, const char *delim){
    char *s = *sp;
    if(!s){
        return NULL;
    }
    char *e = s + strcspn(s, delim);
    if(*e){
        *e = 0;
        *sp = e + 1;
    } else {
        *sp = NULL;
    }
    return s;
}

char *strtok_r(char *s, const char *delim, char **save){
    if(!s){
        s = *save;
    }
    if(!s){
        return NULL;
    }
    s += strspn(s, delim);
    if(!*s){
        *save = NULL;
        return NULL;
    }
    char *e = s + strcspn(s, delim);
    if(*e){
        *e = 0;
        *save = e + 1;
    } else {
        *save = NULL;
    }
    return s;
}

char *strtok(char *s, const char *delim){
    static char *save;
    return strtok_r(s, delim, &save);
}

char *strndup(const char *s, size_t n){
    size_t l = strnlen(s, n);
    char *p = malloc(l + 1);
    if(p){
        memcpy(p, s, l);
        p[l] = 0;
    }
    return p;
}

char *strdup(const char *s){
    return strndup(s, strlen(s));
}

char *strerror(int e){
    switch(e){
        case 0: return "Success";
        case 1: return "Operation not permitted";
        case 2: return "No such file or directory";
        case 4: return "Interrupted system call";
        case 5: return "I/O error";
        case 7: return "Argument list too long";
        case 8: return "Exec format error";
        case 9: return "Bad file descriptor";
        case 10: return "No child processes";
        case 11: return "Resource temporarily unavailable";
        case 12: return "Out of memory";
        case 13: return "Permission denied";
        case 14: return "Bad address";
        case 16: return "Device or resource busy";
        case 17: return "File exists";
        case 18: return "Cross-device link";
        case 19: return "No such device";
        case 20: return "Not a directory";
        case 21: return "Is a directory";
        case 22: return "Invalid argument";
        case 24: return "Too many open files";
        case 25: return "Not a tty";
        case 27: return "File too large";
        case 28: return "No space left on device";
        case 29: return "Illegal seek";
        case 30: return "Read-only file system";
        case 31: return "Too many links";
        case 32: return "Broken pipe";
        case 33: return "Numerical argument out of domain";
        case 34: return "Result too large";
        case 36: return "File name too long";
        case 38: return "Function not implemented";
        case 39: return "Directory not empty";
        case 40: return "Too many levels of symbolic links";
        case 95: return "Operation not supported";
        default: return "Unknown error";
    }
}

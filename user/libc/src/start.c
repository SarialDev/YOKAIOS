#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include "internal.h"

int errno;
char **environ;

#define MAX_ATEXIT 32
static void (*atexit_fns[MAX_ATEXIT])(void);
static int atexit_n;

int atexit(void (*fn)(void)){
    if(atexit_n == MAX_ATEXIT){
        return -1;
    }
    atexit_fns[atexit_n++] = fn;
    return 0;
}

void exit(int code){
    while(atexit_n > 0){
        atexit_fns[--atexit_n]();
    }
    fflush(NULL);
    _exit(code);
}

void _Exit(int code){
    _exit(code);
}

void abort(void){
    fflush(NULL);
    _exit(134);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func){
    fprintf(stderr, "assertion failed: %s (%s:%d, %s)\n", expr, file, line, func);
    abort();
}

// sp -> argc, argv[argc], NULL, envp[], NULL, auxv
void __libc_start_main(int (*main_fn)(int, char **, char **), long *sp){
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    environ = argv + argc + 1;
    exit(main_fn(argc, argv, environ));
}


char *getenv(const char *name){
    size_t n = strlen(name);
    if(!environ || !n || strchr(name, '=')){
        return NULL;
    }
    for(char **e = environ; *e; e++){
        if(strncmp(*e, name, n) == 0 && (*e)[n] == '='){
            return *e + n + 1;
        }
    }
    return NULL;
}

// Tabloul environ propriu (copiat la prima modificare); sirurile noi nu se elibereaza
static int env_owned;

static int env_prepare(size_t extra){
    size_t n = 0;
    while(environ && environ[n]){
        n++;
    }
    char **v = malloc((n + extra + 1) * sizeof(char *));
    if(!v){
        return -1;
    }
    for(size_t i = 0; i < n; i++){
        v[i] = environ[i];
    }
    v[n] = NULL;
    if(env_owned){
        free(environ);
    }
    environ = v;
    env_owned = 1;
    return 0;
}

int putenv(char *s){
    char *eq = strchr(s, '=');
    if(!eq){
        errno = EINVAL;
        return -1;
    }
    size_t n = (size_t)(eq - s);
    for(char **e = environ; e && *e; e++){
        if(strncmp(*e, s, n) == 0 && (*e)[n] == '='){
            *e = s;
            return 0;
        }
    }
    if(env_prepare(1) < 0){
        return -1;
    }
    size_t c = 0;
    while(environ[c]){
        c++;
    }
    environ[c] = s;
    environ[c + 1] = NULL;
    return 0;
}

int setenv(const char *name, const char *value, int overwrite){
    if(!*name || strchr(name, '=')){
        errno = EINVAL;
        return -1;
    }
    if(!overwrite && getenv(name)){
        return 0;
    }
    size_t nl = strlen(name), vl = strlen(value);
    char *s = malloc(nl + vl + 2);
    if(!s){
        return -1;
    }
    memcpy(s, name, nl);
    s[nl] = '=';
    memcpy(s + nl + 1, value, vl + 1);
    return putenv(s);
}

int unsetenv(const char *name){
    size_t n = strlen(name);
    if(!n || strchr(name, '=')){
        errno = EINVAL;
        return -1;
    }
    for(char **e = environ; e && *e; e++){
        if(strncmp(*e, name, n) == 0 && (*e)[n] == '='){
            for(char **f = e; *f; f++){
                *f = f[1];
            }
            e--;
        }
    }
    return 0;
}

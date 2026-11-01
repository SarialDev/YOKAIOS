#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include "internal.h"

extern pid_t wait4(pid_t pid, int *status, int options, void *rusage);
extern long getdents64(int fd, void *buf, size_t n);
extern int umount2(const char *target, int flags);

uid_t getuid(void){ return 0; }
uid_t geteuid(void){ return 0; }
gid_t getgid(void){ return 0; }

pid_t waitpid(pid_t pid, int *status, int options){
    return wait4(pid, status, options, NULL);
}

pid_t wait(int *status){
    return wait4(-1, status, 0, NULL);
}

int creat(const char *path, mode_t mode){
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int execv(const char *path, char *const argv[]){
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[]){
    if(strchr(file, '/')){
        return execv(file, argv);
    }
    const char *path = getenv("PATH");
    if(!path){
        path = "/bin:/sbin:/usr/bin";
    }
    char buf[256];
    int saved = ENOENT;
    while(*path){
        size_t n = strcspn(path, ":");
        if(n + strlen(file) + 2 <= sizeof(buf)){
            memcpy(buf, path, n);
            buf[n] = '/';
            strcpy(buf + n + 1, file);
            execv(buf, argv);
            if(errno != ENOENT){
                saved = errno;
            }
        }
        path += n + (path[n] ? 1 : 0);
    }
    errno = saved;
    return -1;
}

int access(const char *path, int mode){
    struct stat st;
    (void)mode;
    return stat(path, &st);
}

int isatty(int fd){
    struct stat st;
    if(fstat(fd, &st) < 0){
        return 0;
    }
    if(!S_ISCHR(st.st_mode)){
        errno = ENOTTY;
        return 0;
    }
    return 1;
}

int usleep(unsigned usec){
    struct timespec ts = {usec / 1000000, (usec % 1000000) * 1000L};
    return nanosleep(&ts, NULL);
}

unsigned sleep(unsigned sec){
    struct timespec ts = {sec, 0};
    return nanosleep(&ts, NULL) < 0 ? sec : 0;
}

void *sbrk(long inc){
    char *cur = __brk(0);
    if(inc == 0){
        return cur;
    }
    char *want = cur + inc;
    if(__brk(want) != want){
        errno = ENOMEM;
        return (void *)-1;
    }
    return cur;
}

int brk(void *addr){
    if(__brk(addr) != addr){
        errno = ENOMEM;
        return -1;
    }
    return 0;
}

long sysconf(int name){
    switch(name){
        case _SC_PAGESIZE: return 4096;
        case _SC_CLK_TCK: return 100;
        default: errno = EINVAL; return -1;
    }
}

int remove(const char *path){
    struct stat st;
    if(lstat(path, &st) < 0){
        return -1;
    }
    return S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
}

struct _DIR {
    int fd;
    size_t pos, len;
    struct dirent ent;
    char buf[2048];
};

struct kdirent {
    unsigned long d_ino;
    long d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

DIR *opendir(const char *path){
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if(fd < 0){
        return NULL;
    }
    DIR *d = calloc(1, sizeof(DIR));
    if(!d){
        close(fd);
        return NULL;
    }
    d->fd = fd;
    return d;
}

struct dirent *readdir(DIR *d){
    if(d->pos >= d->len){
        long n = getdents64(d->fd, d->buf, sizeof(d->buf));
        if(n <= 0){
            return NULL;
        }
        d->pos = 0;
        d->len = (size_t)n;
    }
    struct kdirent *k = (struct kdirent *)(d->buf + d->pos);
    d->pos += k->d_reclen;
    d->ent.d_ino = k->d_ino;
    d->ent.d_type = k->d_type;
    strlcpy(d->ent.d_name, k->d_name, sizeof(d->ent.d_name));
    return &d->ent;
}

int closedir(DIR *d){
    int r = close(d->fd);
    free(d);
    return r;
}

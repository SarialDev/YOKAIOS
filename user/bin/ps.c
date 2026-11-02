/* ps: procesele din /proc (PID PPID S COMMAND), in ordinea pid-ului */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>

struct proc {
    long pid, ppid;
    char state;
    char cmd[128];
};

static int read_small(const char *path, char *buf, size_t cap){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return -1;
    }
    size_t len = 0;
    while(len + 1 < cap){
        ssize_t n = read(fd, buf + len, cap - 1 - len);
        if(n <= 0){
            break;
        }
        len += (size_t)n;
    }
    close(fd);
    buf[len] = 0;
    return (int)len;
}

static const char *find_field(const char *status, const char *name){
    size_t n = strlen(name);
    for(const char *p = status; *p; ){
        if(strncmp(p, name, n) == 0 && p[n] == ':'){
            p += n + 1;
            while(*p == ' ' || *p == '\t'){
                p++;
            }
            return p;
        }
        const char *nl = strchr(p, '\n');
        if(!nl){
            break;
        }
        p = nl + 1;
    }
    return NULL;
}

static int cmp(const void *a, const void *b){
    long x = ((const struct proc *)a)->pid, y = ((const struct proc *)b)->pid;
    return (x > y) - (x < y);
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: ps");
        return 0;
    }
    if(argc != 1){
        fprintf(stderr, "uz: ps\n");
        return 2;
    }
    DIR *d = opendir("/proc");
    if(!d){
        fprintf(stderr, "ps: /proc indisponibil\n");
        return 1;
    }
    size_t cap = 32, n = 0;
    struct proc *list = malloc(cap * sizeof(*list));
    char *status = malloc(1024);
    if(!list || !status){
        fprintf(stderr, "ps: memorie insuficienta\n");
        return 1;
    }
    struct dirent *e;
    while((e = readdir(d))){
        if(e->d_name[0] < '0' || e->d_name[0] > '9'){
            continue;
        }
        char path[64];
        snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
        if(read_small(path, status, 1024) < 0){
            continue;       // procesul a iesit intre timp
        }
        if(n == cap){
            cap *= 2;
            list = realloc(list, cap * sizeof(*list));
            if(!list){
                return 1;
            }
        }
        struct proc *p = &list[n];
        p->pid = atol(e->d_name);
        const char *f = find_field(status, "PPid");
        p->ppid = f ? atol(f) : 0;
        f = find_field(status, "State");
        p->state = f ? *f : '?';
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        int len = read_small(path, p->cmd, sizeof(p->cmd));
        if(len > 0){
            for(int i = 0; i < len - 1; i++){
                if(p->cmd[i] == 0){
                    p->cmd[i] = ' ';
                }
            }
        } else {
            f = find_field(status, "Name");
            size_t k = 0;
            p->cmd[k++] = '[';
            while(f && *f && *f != '\n' && k < sizeof(p->cmd) - 2){
                p->cmd[k++] = *f++;
            }
            p->cmd[k++] = ']';
            p->cmd[k] = 0;
        }
        n++;
    }
    closedir(d);
    qsort(list, n, sizeof(*list), cmp);
    printf("%5s %5s %s %s\n", "PID", "PPID", "S", "COMMAND");
    for(size_t i = 0; i < n; i++){
        printf("%5ld %5ld %c %s\n", list[i].pid, list[i].ppid, list[i].state, list[i].cmd);
    }
    free(list);
    free(status);
    return 0;
}

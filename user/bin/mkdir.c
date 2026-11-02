/* mkdir [-p] director... */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static int make(const char *path, int parents){
    if(!parents){
        return mkdir(path, 0755);
    }
    char buf[256];
    size_t n = strlen(path);
    if(n >= sizeof(buf)){
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, n + 1);
    for(size_t i = 1; i <= n; i++){
        if(buf[i] == '/' || buf[i] == 0){
            char c = buf[i];
            buf[i] = 0;
            if(mkdir(buf, 0755) < 0 && errno != EEXIST){
                return -1;
            }
            buf[i] = c;
        }
    }
    return 0;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: mkdir [-p] director...");
        return 0;
    }
    int parents = 0, rc = 0, i = 1;
    if(i < argc && strcmp(argv[i], "-p") == 0){
        parents = 1;
        i++;
    }
    if(i >= argc){
        fprintf(stderr, "uz: mkdir [-p] director...\n");
        return 2;
    }
    for(; i < argc; i++){
        if(make(argv[i], parents) < 0){
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

/* cat [fisier...]: fara argumente (sau cu "-") copiaza stdin */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static int copy_fd(int fd, const char *name){
    char buf[4096];
    for(;;){
        ssize_t n = read(fd, buf, sizeof(buf));
        if(n == 0){
            return 0;
        }
        if(n < 0){
            fprintf(stderr, "cat: %s: %s\n", name, strerror(errno));
            return 1;
        }
        for(ssize_t off = 0; off < n;){
            ssize_t w = write(1, buf + off, (size_t)(n - off));
            if(w <= 0){
                fprintf(stderr, "cat: write: %s\n", strerror(errno));
                return 1;
            }
            off += w;
        }
    }
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: cat [fisier...]");
        return 0;
    }
    if(argc < 2){
        return copy_fd(0, "stdin");
    }
    int rc = 0;
    for(int i = 1; i < argc; i++){
        if(strcmp(argv[i], "-") == 0){
            rc |= copy_fd(0, "stdin");
            continue;
        }
        int fd = open(argv[i], O_RDONLY);
        if(fd < 0){
            fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        rc |= copy_fd(fd, argv[i]);
        close(fd);
    }
    return rc;
}

/* insmod FISIER.ko: incarca un modul de kernel semnat (CAP_SYS_ADMIN). Semnatura o verifica kernelul (kernel/core/kmod.h) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/module.h>

int main(int argc, char **argv){
    if(argc != 2){
        fprintf(stderr, "uz: insmod FISIER.ko\n");
        return 2;
    }
    int fd = open(argv[1], O_RDONLY);
    if(fd < 0){
        fprintf(stderr, "insmod: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap);
    for(;;){
        if(!buf){
            fprintf(stderr, "insmod: memorie insuficienta\n");
            return 1;
        }
        if(n == cap){
            cap *= 2;
            if(cap > (1 << 20)){
                fprintf(stderr, "insmod: %s: prea mare\n", argv[1]);
                return 1;
            }
            buf = realloc(buf, cap);
            continue;
        }
        ssize_t r = read(fd, buf + n, cap - n);
        if(r < 0){
            fprintf(stderr, "insmod: %s: %s\n", argv[1], strerror(errno));
            return 1;
        }
        if(r == 0){
            break;
        }
        n += (size_t)r;
    }
    close(fd);
    if(init_module(buf, n) < 0){
        const char *why = errno == ENOKEY ? "modul nesemnat" :
                          errno == EKEYREJECTED ? "semnatura Ed25519 INVALIDA (modul modificat sau cheie necunoscuta)" :
                          errno == ENOEXEC ? "ELF invalid sau relocare nesuportata" :
                          errno == ENOENT ? "simbol nerezolvat (vezi mesajul kernelului)" : strerror(errno);
        fprintf(stderr, "insmod: %s: %s\n", argv[1], why);
        return 1;
    }
    return 0;
}

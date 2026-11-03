/* rmmod NUME: descarca un modul de kernel (CAP_SYS_ADMIN) */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/module.h>

int main(int argc, char **argv){
    if(argc != 2){
        fprintf(stderr, "uz: rmmod NUME\n");
        return 2;
    }
    if(delete_module(argv[1]) < 0){
        fprintf(stderr, "rmmod: %s: %s\n", argv[1], errno == ENOENT ? "nu e incarcat" : strerror(errno));
        return 1;
    }
    return 0;
}

/* umount director... */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: umount director...");
        return 0;
    }
    if(argc < 2){
        fprintf(stderr, "uz: umount director...\n");
        return 2;
    }
    int rc = 0;
    for(int i = 1; i < argc; i++){
        if(umount(argv[i]) < 0){
            fprintf(stderr, "umount: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

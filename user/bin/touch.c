/* touch fisier...: creeaza fisierul daca nu exista (timpul unui fisier existent nu se schimba: nu avem utimensat) */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: touch fisier...");
        return 0;
    }
    if(argc < 2){
        fprintf(stderr, "uz: touch fisier...\n");
        return 2;
    }
    int rc = 0;
    for(int i = 1; i < argc; i++){
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if(fd < 0){
            fprintf(stderr, "touch: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        close(fd);
    }
    return rc;
}

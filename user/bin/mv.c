/* mv sursa destinatie (daca destinatia e director, sursa intra in el) */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

int main(int argc, char **argv){
    if(argc != 3){
        fprintf(stderr, "uz: mv sursa destinatie\n");
        return 2;
    }
    char dst[512];
    struct stat st;
    if(stat(argv[2], &st) == 0 && S_ISDIR(st.st_mode)){
        const char *base = strrchr(argv[1], '/');
        snprintf(dst, sizeof(dst), "%s/%s", argv[2], base ? base + 1 : argv[1]);
    } else {
        snprintf(dst, sizeof(dst), "%s", argv[2]);
    }
    if(rename(argv[1], dst) < 0){
        fprintf(stderr, "mv: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    return 0;
}

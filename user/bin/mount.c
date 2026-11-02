/* mount: fara argumente listeaza montarile; mount [-r] -t tip [-o ro|rw] sursa destinatie */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>

#define MS_RDONLY 1

static int list_mounts(void){
    FILE *f = fopen("/proc/mounts", "r");
    if(!f){
        fprintf(stderr, "mount: /proc/mounts indisponibil\n");
        return 1;
    }
    char line[512];
    while(fgets(line, sizeof(line), f)){
        // "<dev> <cale> <tip> <ro|rw> 0 0"
        char *save, *dev = strtok_r(line, " \n", &save), *path = strtok_r(NULL, " \n", &save);
        char *type = strtok_r(NULL, " \n", &save), *opt = strtok_r(NULL, " \n", &save);
        if(dev && path && type && opt){
            printf("%s on %s type %s (%s)\n", dev, path, type, opt);
        }
    }
    fclose(f);
    return 0;
}

static void usage(void){
    fprintf(stderr, "uz: mount [-r] -t tip [-o ro|rw] sursa destinatie\n");
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: mount [-r] -t tip [-o ro|rw] sursa destinatie (fara argumente: listeaza montarile)");
        return 0;
    }
    if(argc == 1){
        return list_mounts();
    }
    const char *type = NULL;
    unsigned long flags = 0;
    int i = 1;
    for(; i < argc && argv[i][0] == '-'; i++){
        if(strcmp(argv[i], "-r") == 0){
            flags |= MS_RDONLY;
        } else if(strcmp(argv[i], "-t") == 0 && i + 1 < argc){
            type = argv[++i];
        } else if(strcmp(argv[i], "-o") == 0 && i + 1 < argc){
            const char *o = argv[++i];
            if(strcmp(o, "ro") == 0){
                flags |= MS_RDONLY;
            } else if(strcmp(o, "rw") != 0){
                fprintf(stderr, "mount: optiune necunoscuta -o %s\n", o);
                return 2;
            }
        } else {
            fprintf(stderr, "mount: optiune necunoscuta %s\n", argv[i]);
            return 2;
        }
    }
    if(argc - i != 2 || !type){
        usage();
        return 2;
    }
    if(mount(argv[i], argv[i + 1], type, flags, NULL) < 0){
        fprintf(stderr, "mount: %s: %s\n", argv[i + 1], strerror(errno));
        return 1;
    }
    return 0;
}

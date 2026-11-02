/* ls [-a] [-l] [cale...]: o intrare pe linie, in ordine alfabetica */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

static int opt_all, opt_long;

static int cmp(const void *a, const void *b){
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void print_entry(const char *dir, const char *name){
    if(!opt_long){
        puts(name);
        return;
    }
    char path[512];
    if(dir){
        snprintf(path, sizeof(path), "%s/%s", dir, name);
    } else {
        snprintf(path, sizeof(path), "%s", name);
    }
    struct stat st;
    if(lstat(path, &st) < 0){
        printf("?????????? %8s %s\n", "?", name);
        return;
    }
    char m[11];
    mode_t md = st.st_mode;
    m[0] = S_ISDIR(md) ? 'd' : S_ISLNK(md) ? 'l' : S_ISCHR(md) ? 'c' : S_ISBLK(md) ? 'b' : S_ISFIFO(md) ? 'p' : '-';
    const char *rwx = "rwxrwxrwx";
    for(int i = 0; i < 9; i++){
        m[1 + i] = (md & (0400 >> i)) ? rwx[i] : '-';
    }
    m[10] = 0;
    printf("%s %8ld %s\n", m, (long)st.st_size, name);
}

static int list_dir(const char *path){
    DIR *d = opendir(path);
    if(!d){
        return -1;
    }
    size_t cap = 64, n = 0;
    char **names = malloc(cap * sizeof(char *));
    struct dirent *e;
    while(names && (e = readdir(d))){
        if(!opt_all && e->d_name[0] == '.'){
            continue;
        }
        if(n == cap){
            cap *= 2;
            names = realloc(names, cap * sizeof(char *));
            if(!names){
                break;
            }
        }
        names[n] = strdup(e->d_name);
        if(names[n]){
            n++;
        }
    }
    closedir(d);
    if(!names){
        return -1;
    }
    qsort(names, n, sizeof(char *), cmp);
    for(size_t i = 0; i < n; i++){
        print_entry(path, names[i]);
        free(names[i]);
    }
    free(names);
    return 0;
}

int main(int argc, char **argv){
    int i = 1;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++){
        for(const char *o = argv[i] + 1; *o; o++){
            if(*o == 'a'){
                opt_all = 1;
            } else if(*o == 'l'){
                opt_long = 1;
            } else {
                fprintf(stderr, "ls: optiune necunoscuta -%c\n", *o);
                return 2;
            }
        }
    }
    int rc = 0;
    int count = argc - i;
    if(count == 0){
        return list_dir(".") < 0 ? (fprintf(stderr, "ls: .: %s\n", strerror(errno)), 1) : 0;
    }
    for(int k = i; k < argc; k++){
        struct stat st;
        if(stat(argv[k], &st) < 0){
            fprintf(stderr, "ls: %s: %s\n", argv[k], strerror(errno));
            rc = 1;
            continue;
        }
        if(!S_ISDIR(st.st_mode)){
            print_entry(NULL, argv[k]);
            continue;
        }
        if(count > 1){
            printf("%s:\n", argv[k]);
        }
        if(list_dir(argv[k]) < 0){
            fprintf(stderr, "ls: %s: %s\n", argv[k], strerror(errno));
            rc = 1;
        }
        if(count > 1 && k + 1 < argc){
            putchar('\n');
        }
    }
    return rc;
}

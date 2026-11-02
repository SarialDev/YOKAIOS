/* rm [-r] [-f] cale... */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

static int force;

static int remove_tree(const char *path, int recursive){
    struct stat st;
    if(lstat(path, &st) < 0){
        return force && errno == ENOENT ? 0 : -1;
    }
    if(!S_ISDIR(st.st_mode)){
        return unlink(path);
    }
    if(!recursive){
        errno = EISDIR;
        return -1;
    }
    DIR *d = opendir(path);
    if(!d){
        return -1;
    }
    int rc = 0;
    struct dirent *e;
    while(rc == 0 && (e = readdir(d))){
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0){
            continue;
        }
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        rc = remove_tree(child, 1);
        // Intrarile sterse intre timp pot muta pozitia in director: reia de la capat
        if(rc == 0){
            closedir(d);
            d = opendir(path);
            if(!d){
                return -1;
            }
        }
    }
    closedir(d);
    return rc ? rc : rmdir(path);
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: rm [-r] [-f] cale...");
        return 0;
    }
    int recursive = 0, rc = 0, i = 1;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++){
        for(const char *o = argv[i] + 1; *o; o++){
            if(*o == 'r' || *o == 'R'){
                recursive = 1;
            } else if(*o == 'f'){
                force = 1;
            } else {
                fprintf(stderr, "rm: optiune necunoscuta -%c\n", *o);
                return 2;
            }
        }
    }
    if(i >= argc){
        fprintf(stderr, "uz: rm [-rf] cale...\n");
        return 2;
    }
    for(; i < argc; i++){
        if(remove_tree(argv[i], recursive) < 0){
            fprintf(stderr, "rm: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

/* cp [-r] sursa... destinatie */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

static int copy_file(const char *src, const char *dst, mode_t mode){
    int in = open(src, O_RDONLY);
    if(in < 0){
        fprintf(stderr, "cp: %s: %s\n", src, strerror(errno));
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if(out < 0){
        fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
        close(in);
        return 1;
    }
    char buf[4096];
    int rc = 0;
    for(;;){
        ssize_t n = read(in, buf, sizeof(buf));
        if(n == 0){
            break;
        }
        if(n < 0){
            fprintf(stderr, "cp: %s: %s\n", src, strerror(errno));
            rc = 1;
            break;
        }
        for(ssize_t off = 0; off < n;){
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if(w <= 0){
                fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
                rc = 1;
                goto done;
            }
            off += w;
        }
    }
done:
    close(in);
    close(out);
    return rc;
}

static int copy_tree(const char *src, const char *dst, int recursive){
    struct stat st, dt;
    if(stat(src, &st) < 0){
        fprintf(stderr, "cp: %s: %s\n", src, strerror(errno));
        return 1;
    }
    if(stat(dst, &dt) == 0 && dt.st_dev == st.st_dev && dt.st_ino == st.st_ino && !S_ISDIR(st.st_mode)){
        fprintf(stderr, "cp: %s si %s sunt acelasi fisier\n", src, dst);
        return 1;
    }
    if(!S_ISDIR(st.st_mode)){
        return copy_file(src, dst, st.st_mode);
    }
    if(!recursive){
        fprintf(stderr, "cp: %s este director (foloseste -r)\n", src);
        return 1;
    }
    size_t sl = strlen(src);
    if(strncmp(dst, src, sl) == 0 && (dst[sl] == '/' || dst[sl] == 0)){
        fprintf(stderr, "cp: nu se poate copia %s in sine\n", src);
        return 1;
    }
    if(mkdir(dst, st.st_mode & 0777) < 0 && errno != EEXIST){
        fprintf(stderr, "cp: %s: %s\n", dst, strerror(errno));
        return 1;
    }
    DIR *d = opendir(src);
    if(!d){
        fprintf(stderr, "cp: %s: %s\n", src, strerror(errno));
        return 1;
    }
    int rc = 0;
    struct dirent *e;
    while((e = readdir(d))){
        if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0){
            continue;
        }
        char s[512], t[512];
        snprintf(s, sizeof(s), "%s/%s", src, e->d_name);
        snprintf(t, sizeof(t), "%s/%s", dst, e->d_name);
        rc |= copy_tree(s, t, 1);
    }
    closedir(d);
    return rc;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: cp [-r] sursa... destinatie");
        return 0;
    }
    int recursive = 0, i = 1;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++){
        for(const char *o = argv[i] + 1; *o; o++){
            if(*o == 'r' || *o == 'R'){
                recursive = 1;
            } else {
                fprintf(stderr, "cp: optiune necunoscuta -%c\n", *o);
                return 2;
            }
        }
    }
    if(argc - i < 2){
        fprintf(stderr, "uz: cp [-r] sursa... destinatie\n");
        return 2;
    }
    const char *dest = argv[argc - 1];
    struct stat dt;
    int dest_dir = stat(dest, &dt) == 0 && S_ISDIR(dt.st_mode);
    if(argc - i > 2 && !dest_dir){
        fprintf(stderr, "cp: %s nu este director\n", dest);
        return 1;
    }
    int rc = 0;
    for(; i < argc - 1; i++){
        char target[512];
        if(dest_dir){
            const char *b = strrchr(argv[i], '/');
            if(b && b[1] == 0){      // "dir/" -> numele fara slash-ul final
                size_t n = strlen(argv[i]);
                while(n > 1 && argv[i][n - 1] == '/'){
                    n--;
                }
                size_t k = n;
                while(k > 0 && argv[i][k - 1] != '/'){
                    k--;
                }
                snprintf(target, sizeof(target), "%s/%.*s", dest, (int)(n - k), argv[i] + k);
            } else {
                snprintf(target, sizeof(target), "%s/%s", dest, b ? b + 1 : argv[i]);
            }
        } else {
            snprintf(target, sizeof(target), "%s", dest);
        }
        rc |= copy_tree(argv[i], target, recursive);
    }
    return rc;
}

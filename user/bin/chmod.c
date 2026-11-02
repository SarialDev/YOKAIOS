/* chmod [-R] MOD fisier...: MOD octal (755, 4755) sau simbolic [ugoa]*[+-=][rwxXst]*, separat prin virgule (u+x,go-w) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *spec;
static int recursive;

/* Mode-ul nou pentru un fisier cu mode-ul vechi old; -1 daca spec e invalid */
static long apply_spec(const char *s, mode_t old){
    if(*s >= '0' && *s <= '7'){
        char *end;
        long v = strtol(s, &end, 8);
        return (*end || v > 07777) ? -1 : v;
    }
    mode_t m = old & 07777;
    mode_t mask = umask(0);
    umask(mask);
    while(*s){
        mode_t who = 0;
        for(; *s && strchr("ugoa", *s); s++){
            who |= *s == 'u' ? 04700 : *s == 'g' ? 02070 : *s == 'o' ? 01007 : 07777;
        }
        int all = who == 0;                     // fara u/g/o/a: ca 'a', dar respecta umask
        if(all){
            who = 07777;
        }
        if(!*s || !strchr("+-=", *s)){
            return -1;
        }
        while(*s && strchr("+-=", *s)){
            char op = *s++;
            mode_t bits = 0;
            for(; *s && strchr("rwxXst", *s); s++){
                switch(*s){
                case 'r': bits |= 0444; break;
                case 'w': bits |= 0222; break;
                case 'x': bits |= 0111; break;
                case 'X': if(S_ISDIR(old) || (old & 0111)){ bits |= 0111; } break;
                case 's': bits |= S_ISUID | S_ISGID; break;
                case 't': bits |= S_ISVTX; break;
                }
            }
            bits &= who;
            if(all){
                bits &= ~mask;
            }
            if(op == '+'){
                m |= bits;
            } else if(op == '-'){
                m &= ~bits;
            } else {
                m = (m & ~who) | bits;
            }
        }
        if(*s == ','){
            s++;
        } else if(*s){
            return -1;
        }
    }
    return m;
}

static int do_path(const char *path){
    struct stat st;
    if(stat(path, &st) < 0){
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        return 1;
    }
    long m = apply_spec(spec, st.st_mode);
    if(chmod(path, (mode_t)m) < 0){
        fprintf(stderr, "chmod: %s: %s\n", path, strerror(errno));
        return 1;
    }
    int rc = 0;
    if(recursive && S_ISDIR(st.st_mode)){
        DIR *d = opendir(path);
        struct dirent *e;
        while(d && (e = readdir(d))){
            if(strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0){
                continue;
            }
            char sub[512];
            snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
            struct stat ls;
            if(lstat(sub, &ls) == 0 && S_ISLNK(ls.st_mode)){
                continue;                       // ca GNU: legaturile simbolice din arbore nu se urmeaza
            }
            rc |= do_path(sub);
        }
        if(d){
            closedir(d);
        }
    }
    return rc;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: chmod [-R] MOD fisier...");
        return 0;
    }
    int i = 1;
    if(i < argc && strcmp(argv[i], "-R") == 0){
        recursive = 1;
        i++;
    }
    if(argc - i < 2){
        fprintf(stderr, "uz: chmod [-R] MOD fisier...\n");
        return 2;
    }
    spec = argv[i++];
    if(apply_spec(spec, S_IFDIR | 0755) < 0){
        fprintf(stderr, "chmod: mod invalid: %s\n", spec);
        return 2;
    }
    int rc = 0;
    for(; i < argc; i++){
        rc |= do_path(argv[i]);
    }
    return rc;
}

/* chown [-R] [-h] [UTILIZATOR][:GRUP] fisier...: nume din /etc/passwd, /etc/group sau numere; -h = legatura insasi */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>

static uid_t uid = (uid_t)-1;
static gid_t gid = (gid_t)-1;
static int recursive, nofollow;

static int parse_num(const char *s, unsigned *out){
    char *end;
    if(!*s){
        return 0;
    }
    unsigned long v = strtoul(s, &end, 10);
    *out = (unsigned)v;
    return *end == 0;
}

static int do_path(const char *path){
    if((nofollow ? lchown(path, uid, gid) : chown(path, uid, gid)) < 0){
        fprintf(stderr, "chown: %s: %s\n", path, strerror(errno));
        return 1;
    }
    struct stat st;
    int rc = 0;
    if(recursive && lstat(path, &st) == 0 && S_ISDIR(st.st_mode)){
        DIR *d = opendir(path);
        struct dirent *e;
        while(d && (e = readdir(d))){
            if(strcmp(e->d_name, ".") && strcmp(e->d_name, "..")){
                char sub[512];
                snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
                int save = nofollow;
                nofollow = 1;                   // in arbore: legaturile se schimba ele insele
                rc |= do_path(sub);
                nofollow = save;
            }
        }
        if(d){
            closedir(d);
        }
    }
    return rc;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: chown [-R] [-h] [UTILIZATOR][:GRUP] fisier...");
        return 0;
    }
    int i = 1;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++){
        for(const char *o = argv[i] + 1; *o; o++){
            if(*o == 'R'){
                recursive = 1;
            } else if(*o == 'h'){
                nofollow = 1;
            } else {
                fprintf(stderr, "chown: optiune necunoscuta -%c\n", *o);
                return 2;
            }
        }
    }
    if(argc - i < 2){
        fprintf(stderr, "uz: chown [-R] [-h] [UTILIZATOR][:GRUP] fisier...\n");
        return 2;
    }
    char spec[128];
    snprintf(spec, sizeof(spec), "%s", argv[i++]);
    char *colon = strchr(spec, ':');
    char *gname = NULL;
    if(colon){
        *colon = 0;
        gname = colon + 1;
    }
    if(*spec){
        struct passwd *pw = getpwnam(spec);
        unsigned v;
        if(pw){
            uid = pw->pw_uid;
            if(gname && !*gname){
                gid = pw->pw_gid;               // "utilizator:" = si grupul lui principal
            }
        } else if(parse_num(spec, &v)){
            uid = v;
        } else {
            fprintf(stderr, "chown: utilizator necunoscut: %s\n", spec);
            return 1;
        }
    }
    if(gname && *gname){
        struct group *gr = getgrnam(gname);
        unsigned v;
        if(gr){
            gid = gr->gr_gid;
        } else if(parse_num(gname, &v)){
            gid = v;
        } else {
            fprintf(stderr, "chown: grup necunoscut: %s\n", gname);
            return 1;
        }
    }
    int rc = 0;
    for(; i < argc; i++){
        rc |= do_path(argv[i]);
    }
    return rc;
}

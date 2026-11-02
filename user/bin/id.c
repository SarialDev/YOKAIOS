/* id [-u|-g|-G] [-n] [-r] [utilizator]: identitatea procesului (sau a utilizatorului din /etc/passwd), ca GNU id */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>

#define MAXG 33

static void put_user(uid_t u){
    struct passwd *pw = getpwuid(u);
    printf("%u", u);
    if(pw){
        printf("(%s)", pw->pw_name);
    }
}

static void put_group(gid_t g){
    struct group *gr = getgrgid(g);
    printf("%u", g);
    if(gr){
        printf("(%s)", gr->gr_name);
    }
}

static void put_name_or_id(int name, int is_user, unsigned id){
    if(name){
        const char *n = NULL;
        if(is_user){
            struct passwd *pw = getpwuid(id);
            n = pw ? pw->pw_name : NULL;
        } else {
            struct group *gr = getgrgid(id);
            n = gr ? gr->gr_name : NULL;
        }
        if(n){
            fputs(n, stdout);
            return;
        }
    }
    printf("%u", id);
}

/* Grupurile din /etc/group care il listeaza pe user (pentru `id utilizator`) */
static int groups_of(const char *user, gid_t base, gid_t *out){
    int n = 0;
    out[n++] = base;
    struct group *gr;
    setgrent();
    while(n < MAXG && (gr = getgrent())){
        if(gr->gr_gid == base){
            continue;
        }
        for(char **m = gr->gr_mem; *m; m++){
            if(strcmp(*m, user) == 0){
                out[n++] = gr->gr_gid;
                break;
            }
        }
    }
    endgrent();
    return n;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: id [-u|-g|-G] [-n] [-r] [utilizator]");
        return 0;
    }
    char what = 0;
    int name = 0, real = 0, i = 1;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++){
        for(const char *o = argv[i] + 1; *o; o++){
            if(*o == 'u' || *o == 'g' || *o == 'G'){
                what = *o;
            } else if(*o == 'n'){
                name = 1;
            } else if(*o == 'r'){
                real = 1;
            } else {
                fprintf(stderr, "id: optiune necunoscuta -%c\n", *o);
                return 2;
            }
        }
    }
    if(i + 1 < argc || ((name || real) && !what)){
        fprintf(stderr, "uz: id [-u|-g|-G] [-n] [-r] [utilizator]\n");
        return 2;
    }
    uid_t uid, euid;
    gid_t gid, egid;
    gid_t groups[MAXG];
    int ng;
    if(i < argc){
        struct passwd *pw = getpwnam(argv[i]);
        if(!pw){
            fprintf(stderr, "id: %s: utilizator necunoscut\n", argv[i]);
            return 1;
        }
        uid = euid = pw->pw_uid;
        gid = egid = pw->pw_gid;
        char user[64];
        snprintf(user, sizeof(user), "%s", pw->pw_name);
        ng = groups_of(user, gid, groups);
    } else {
        uid = getuid();
        euid = geteuid();
        gid = getgid();
        egid = getegid();
        gid_t extra[MAXG - 1];
        int n = getgroups(MAXG - 1, extra);
        ng = 0;
        groups[ng++] = egid;
        for(int k = 0; k < n; k++){
            if(extra[k] != egid){
                groups[ng++] = extra[k];
            }
        }
    }
    if(what == 'u' || what == 'g'){
        unsigned v = what == 'u' ? (real ? uid : euid) : (real ? gid : egid);
        put_name_or_id(name, what == 'u', v);
        putchar('\n');
        return 0;
    }
    if(what == 'G'){
        for(int k = 0; k < ng; k++){
            if(k){
                putchar(' ');
            }
            put_name_or_id(name, 0, groups[k]);
        }
        putchar('\n');
        return 0;
    }
    printf("uid=");
    put_user(uid);
    printf(" gid=");
    put_group(gid);
    if(euid != uid){
        printf(" euid=");
        put_user(euid);
    }
    if(egid != gid){
        printf(" egid=");
        put_group(egid);
    }
    printf(" groups=");
    for(int k = 0; k < ng; k++){
        if(k){
            putchar(',');
        }
        put_group(groups[k]);
    }
    putchar('\n');
    return 0;
}

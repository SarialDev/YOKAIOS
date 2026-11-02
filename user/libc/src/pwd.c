/* getpwnam/getpwuid/getgrnam/getgrgid/initgroups: citesc /etc/passwd si /etc/group linie cu linie */
#include <pwd.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_MAX_PW 256
#define GR_MEM_MAX 32

static char pw_line[LINE_MAX_PW];
static struct passwd pw_ent;
static char gr_line[LINE_MAX_PW];
static struct group gr_ent;
static char *gr_mem[GR_MEM_MAX + 1];

/* Imparte line la ':' in cel mult n campuri; numarul de campuri gasite */
static int split(char *line, char **f, int n){
    line[strcspn(line, "\r\n")] = 0;
    int k = 0;
    f[k++] = line;
    for(char *p = line; *p && k < n; p++){
        if(*p == ':'){
            *p = 0;
            f[k++] = p + 1;
        }
    }
    return k;
}

static int parse_id(const char *s, unsigned *out){
    char *end;
    if(!*s){
        return 0;
    }
    unsigned long v = strtoul(s, &end, 10);
    *out = (unsigned)v;
    return *end == 0;
}

/* Prima intrare din /etc/passwd cu numele name (sau uid-ul uid daca name e NULL) */
static struct passwd *pw_find(const char *name, uid_t uid){
    FILE *f = fopen("/etc/passwd", "r");
    if(!f){
        return NULL;
    }
    struct passwd *r = NULL;
    while(!r && fgets(pw_line, sizeof(pw_line), f)){
        char *fl[7];
        unsigned u, g;
        if(pw_line[0] == '#' || split(pw_line, fl, 7) != 7 || !parse_id(fl[2], &u) || !parse_id(fl[3], &g)){
            continue;
        }
        if(name ? strcmp(fl[0], name) == 0 : u == uid){
            pw_ent = (struct passwd){fl[0], fl[1], u, g, fl[4], fl[5], fl[6]};
            r = &pw_ent;
        }
    }
    fclose(f);
    return r;
}

struct passwd *getpwnam(const char *name){
    return name ? pw_find(name, 0) : NULL;
}

struct passwd *getpwuid(uid_t uid){
    return pw_find(NULL, uid);
}

/* Urmatoarea intrare valida din /etc/group (f deschis) in gr_ent; NULL la sfarsit */
static struct group *gr_next(FILE *f){
    while(fgets(gr_line, sizeof(gr_line), f)){
        char *fl[4];
        unsigned g;
        if(gr_line[0] == '#' || split(gr_line, fl, 4) != 4 || !parse_id(fl[2], &g)){
            continue;
        }
        int n = 0;
        for(char *p = fl[3]; *p && n < GR_MEM_MAX;){
            gr_mem[n++] = p;
            p += strcspn(p, ",");
            if(*p){
                *p++ = 0;
            }
        }
        gr_mem[n] = NULL;
        gr_ent = (struct group){fl[0], fl[1], g, gr_mem};
        return &gr_ent;
    }
    return NULL;
}

static struct group *gr_find(const char *name, gid_t gid){
    FILE *f = fopen("/etc/group", "r");
    if(!f){
        return NULL;
    }
    struct group *g;
    while((g = gr_next(f)) && !(name ? strcmp(g->gr_name, name) == 0 : g->gr_gid == gid)){
    }
    fclose(f);
    return g;
}

struct group *getgrnam(const char *name){
    return name ? gr_find(name, 0) : NULL;
}

struct group *getgrgid(gid_t gid){
    return gr_find(NULL, gid);
}

static FILE *gr_iter;

void setgrent(void){
    endgrent();
    gr_iter = fopen("/etc/group", "r");
}

struct group *getgrent(void){
    if(!gr_iter){
        gr_iter = fopen("/etc/group", "r");
    }
    return gr_iter ? gr_next(gr_iter) : NULL;
}

void endgrent(void){
    if(gr_iter){
        fclose(gr_iter);
        gr_iter = NULL;
    }
}

int initgroups(const char *user, gid_t gid){
    gid_t list[32];
    size_t n = 0;
    list[n++] = gid;
    FILE *f = fopen("/etc/group", "r");
    if(f){
        struct group *g;
        while((g = gr_next(f)) && n < 32){
            for(char **m = g->gr_mem; *m; m++){
                if(strcmp(*m, user) == 0 && g->gr_gid != gid){
                    list[n++] = g->gr_gid;
                    break;
                }
            }
        }
        fclose(f);
    }
    return setgroups(n, list);
}

/* Variabilele shell-ului: nume -> valoare + steag de export. Mediul copiilor se construieste din cele exportate */
#include "ysh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

struct var {
    char *name;
    char *value;
    int exported;
};

static struct var *vars;
static int nvars, capvars;
int last_status;

int valid_name(const char *s, size_t n){
    if(n == 0 || !(isalpha((unsigned char)s[0]) || s[0] == '_')){
        return 0;
    }
    for(size_t i = 1; i < n; i++){
        if(!(isalnum((unsigned char)s[i]) || s[i] == '_')){
            return 0;
        }
    }
    return 1;
}

static struct var *find(const char *name){
    for(int i = 0; i < nvars; i++){
        if(strcmp(vars[i].name, name) == 0){
            return &vars[i];
        }
    }
    return NULL;
}

const char *var_get(const char *name){
    struct var *v = find(name);
    return v ? v->value : NULL;
}

int var_set(const char *name, const char *value, int exported){
    struct var *v = find(name);
    if(!v){
        if(nvars == capvars){
            int ncap = capvars ? capvars * 2 : 32;
            struct var *nv = realloc(vars, (size_t)ncap * sizeof(struct var));
            if(!nv){
                return -1;
            }
            vars = nv;
            capvars = ncap;
        }
        v = &vars[nvars];
        v->name = strdup(name);
        v->value = NULL;
        v->exported = 0;
        if(!v->name){
            return -1;
        }
        nvars++;
    }
    char *copy = strdup(value);
    if(!copy){
        return -1;
    }
    free(v->value);
    v->value = copy;
    if(exported >= 0){
        v->exported = exported;
    }
    return 0;
}

void var_unset(const char *name){
    for(int i = 0; i < nvars; i++){
        if(strcmp(vars[i].name, name) == 0){
            free(vars[i].name);
            free(vars[i].value);
            vars[i] = vars[--nvars];
            return;
        }
    }
}

void var_mark_exported(const char *name){
    struct var *v = find(name);
    if(v){
        v->exported = 1;
    } else {
        var_set(name, "", 1);
    }
}

void vars_init(void){
    for(char **e = environ; e && *e; e++){
        char *eq = strchr(*e, '=');
        if(!eq || eq == *e){
            continue;
        }
        char name[64];
        size_t n = (size_t)(eq - *e);
        if(n >= sizeof(name)){
            continue;
        }
        memcpy(name, *e, n);
        name[n] = 0;
        var_set(name, eq + 1, 1);
    }
}

char **vars_envp(void){
    char **envp = calloc((size_t)nvars + 1, sizeof(char *));
    if(!envp){
        return NULL;
    }
    int k = 0;
    for(int i = 0; i < nvars; i++){
        if(!vars[i].exported){
            continue;
        }
        size_t nl = strlen(vars[i].name), vl = strlen(vars[i].value);
        char *s = malloc(nl + vl + 2);
        if(!s){
            continue;
        }
        memcpy(s, vars[i].name, nl);
        s[nl] = '=';
        memcpy(s + nl + 1, vars[i].value, vl + 1);
        envp[k++] = s;
    }
    envp[k] = NULL;
    return envp;
}

void envp_free(char **envp){
    if(!envp){
        return;
    }
    for(char **e = envp; *e; e++){
        free(*e);
    }
    free(envp);
}

void vars_print(int only_exported){
    for(int i = 0; i < nvars; i++){
        if(only_exported && !vars[i].exported){
            continue;
        }
        if(only_exported){
            printf("export %s=\"%s\"\n", vars[i].name, vars[i].value);
        } else {
            printf("%s=%s\n", vars[i].name, vars[i].value);
        }
    }
}

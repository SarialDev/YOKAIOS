/* ypanel: /etc/ypanel.conf. Linie: Nume | comanda | optiuni  ('#' = comentariu). Optiuni: pin = aplicatia e ancorata in bara.
 * Meniul Start le arata pe toate, in ordinea din fisier. Fara fisier: Terminal ancorat + cele doua demo-uri. */
#include "panel.h"
#include <stdio.h>
#include <string.h>

static char *trim(char *s){
    while(*s == ' ' || *s == '\t'){
        s++;
    }
    size_t n = strlen(s);
    while(n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')){
        s[--n] = 0;
    }
    return s;
}

static void add_app(const char *name, const char *cmd, int pin){
    if(P.napps >= MAX_APPS || !name[0] || !cmd[0]){
        return;
    }
    App *a = &P.apps[P.napps];
    snprintf(a->name, sizeof(a->name), "%s", name);
    snprintf(a->cmd, sizeof(a->cmd), "%s", cmd);
    a->pin = pin;
    if(pin){
        P.pins[P.npins++] = P.napps;
    }
    P.napps++;
}

int conf_load(const char *path){
    P.napps = P.npins = 0;
    FILE *f = fopen(path, "r");
    if(!f){
        add_app("Terminal", "/bin/yterm", 1);
        add_app("Ceas", "/bin/ywdemo -m clock -t Ceas", 0);
        add_app("Bile", "/bin/ywdemo -m balls -t Bile", 0);
        return P.napps;
    }
    char line[256];
    while(fgets(line, sizeof(line), f)){
        char *l = trim(line);
        if(!*l || *l == '#'){
            continue;
        }
        char *name = l;
        char *bar = strchr(l, '|');
        if(!bar){
            continue;
        }
        *bar = 0;
        char *cmd = bar + 1;
        char *opts = "";
        bar = strchr(cmd, '|');
        if(bar){
            *bar = 0;
            opts = bar + 1;
        }
        name = trim(name);
        cmd = trim(cmd);
        opts = trim(opts);
        add_app(name, cmd, strstr(opts, "pin") != NULL);
    }
    fclose(f);
    return P.napps;
}

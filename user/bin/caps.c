/* caps [-d lista] [comanda arg...]: fara comanda tipareste capabilitatile procesului; cu comanda le reduce
 * (-d scoate lista din permitted si effective: rt,net,... sau "all") si porneste comanda. Capabilitatile nu pot creste */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/capability.h>

static const struct { const char *name; unsigned long long bit; } names[] = {
    {"sys_admin", CAP_SYS_ADMIN}, {"net", CAP_NET}, {"rawio", CAP_RAWIO}, {"rt", CAP_RT},
    {"mount", CAP_MOUNT}, {"kill", CAP_KILL}, {"setuid", CAP_SETUID},
};
#define NNAMES (sizeof(names) / sizeof(names[0]))

static void show(unsigned long long mask){
    int n = 0;
    for(unsigned i = 0; i < NNAMES; i++){
        if(mask & names[i].bit){
            printf("%s%s", n++ ? "," : "", names[i].name);
        }
    }
    if(!n){
        printf("none");
    }
}

/* lista "rt,net" / "all" -> masca; 0 la nume necunoscut */
static int parse(const char *list, unsigned long long *out){
    char buf[128];
    if(strlen(list) >= sizeof(buf)){
        return 0;
    }
    strcpy(buf, list);
    *out = 0;
    for(char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")){
        if(strcmp(tok, "all") == 0){
            *out |= CAP_ALL;
            continue;
        }
        int ok = 0;
        for(unsigned i = 0; i < NNAMES; i++){
            if(strcmp(tok, names[i].name) == 0){
                *out |= names[i].bit;
                ok = 1;
            }
        }
        if(!ok){
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv){
    int i = 1;
    unsigned long long drop = 0;
    if(i < argc && strcmp(argv[i], "-d") == 0){
        if(i + 1 >= argc || !parse(argv[i + 1], &drop)){
            fprintf(stderr, "caps: lista invalida (sys_admin net rawio rt mount kill setuid all)\n");
            return 2;
        }
        i += 2;
    }
    struct cap_state st;
    if(capget(0, &st) < 0){
        fprintf(stderr, "caps: capget: %s\n", strerror(errno));
        return 1;
    }
    if(drop){
        st.permitted &= ~drop;
        st.effective &= ~drop;
        if(capset(&st) < 0){
            fprintf(stderr, "caps: capset: %s\n", strerror(errno));
            return 1;
        }
    }
    if(i >= argc){
        printf("permitted: ");
        show(st.permitted);
        printf("\neffective: ");
        show(st.effective);
        printf("\n");
        return 0;
    }
    execvp(argv[i], argv + i);
    fprintf(stderr, "caps: %s: %s\n", argv[i], strerror(errno));
    return errno == ENOENT ? 127 : 126;
}

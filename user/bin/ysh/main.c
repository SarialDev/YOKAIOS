/* ysh: bucla principala. Uz: ysh [-l] | ysh -c "comanda" | ysh < fisier */
#include "ysh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

/* "yokai@drone_terminal:<cwd>$ " ; HOME se scrie ~ */
void prompt_build(char *buf, size_t cap){
    char cwd[PATH_MAX];
    if(!getcwd(cwd, sizeof(cwd))){
        strcpy(cwd, "?");
    }
    const char *home = var_get("HOME");
    const char *shown = cwd;
    char tilde[PATH_MAX];
    if(home && *home && strcmp(home, "/") != 0){
        size_t hl = strlen(home);
        if(strncmp(cwd, home, hl) == 0 && (cwd[hl] == 0 || cwd[hl] == '/')){
            snprintf(tilde, sizeof(tilde), "~%s", cwd + hl);
            shown = tilde;
        }
    }
    snprintf(buf, cap, "yokai@drone_terminal:%s$ ", shown);
}

int execute_line(const char *line){
    // struct list e mare (zeci de KiB): pe heap, stiva user are doar 64 KiB
    struct list *l = malloc(sizeof(*l));
    char err[96];
    if(!l){
        fprintf(stderr, "ysh: out of memory\n");
        return 1;
    }
    if(parse_line(line, l, err, sizeof(err)) < 0){
        fprintf(stderr, "ysh: %s\n", err);
        last_status = 2;
        free(l);
        return 2;
    }
    int st = l->n ? run_list(l) : last_status;
    list_free(l);
    free(l);
    fflush(NULL);
    return st;
}

static void login_banner(void){
    printf("YOKAI OS version 0.1 - ysh %s (type `help`)\n", YSH_VERSION);
    FILE *f = fopen("/etc/motd", "r");
    if(f){
        char line[160];
        while(fgets(line, sizeof(line), f)){
            fputs(line, stdout);
        }
        fclose(f);
    }
}

int main(int argc, char **argv){
    vars_init();
    if(!var_get("PATH")){
        var_set("PATH", "/bin:/sbin", 1);
    }
    if(!var_get("HOME")){
        var_set("HOME", "/", 1);
    }
    char cwd[PATH_MAX];
    if(getcwd(cwd, sizeof(cwd))){
        var_set("PWD", cwd, 1);
    }
    var_set("SHELL", "/bin/ysh", 1);

    if(argc >= 3 && strcmp(argv[1], "-c") == 0){
        return execute_line(argv[2]);
    }
    int login = argc > 1 && strcmp(argv[1], "-l") == 0;
    int interactive = isatty(0);
    if(login && interactive){
        login_banner();
    }

    char line[LINE_CAP];
    char prompt[PATH_MAX + 32];
    for(;;){
        reap_jobs();
        fflush(NULL);
        prompt_build(prompt, sizeof(prompt));
        int n = line_read(prompt, line, sizeof(line));
        if(n < 0){
            if(interactive){
                puts("exit");
            }
            break;
        }
        if(n == 0){
            continue;
        }
        history_add(line);
        execute_line(line);
    }
    fflush(NULL);
    return last_status;
}

/* Comenzile interne: cd pwd exit export unset history clear version help */
#include "ysh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

typedef int (*builtin_fn)(int argc, char **argv);

static int b_cd(int argc, char **argv){
    const char *target;
    if(argc > 2){
        fprintf(stderr, "ysh: cd: too many arguments\n");
        return 1;
    }
    if(argc == 2 && strcmp(argv[1], "-") == 0){
        target = var_get("OLDPWD");
        if(!target){
            fprintf(stderr, "ysh: cd: OLDPWD not set\n");
            return 1;
        }
        printf("%s\n", target);
    } else if(argc == 2){
        target = argv[1];
    } else {
        target = var_get("HOME");
        if(!target){
            fprintf(stderr, "ysh: cd: HOME not set\n");
            return 1;
        }
    }
    char old[PATH_MAX];
    if(!getcwd(old, sizeof(old))){
        old[0] = 0;
    }
    if(chdir(target) < 0){
        fprintf(stderr, "ysh: cd: %s: %s\n", target, strerror(errno));
        return 1;
    }
    char now[PATH_MAX];
    if(old[0]){
        var_set("OLDPWD", old, 1);
    }
    if(getcwd(now, sizeof(now))){
        var_set("PWD", now, 1);
    }
    return 0;
}

static int b_pwd(int argc, char **argv){
    (void)argc;
    (void)argv;
    char buf[PATH_MAX];
    if(!getcwd(buf, sizeof(buf))){
        fprintf(stderr, "ysh: pwd: %s\n", strerror(errno));
        return 1;
    }
    puts(buf);
    return 0;
}

static int b_exit(int argc, char **argv){
    int code = last_status;
    if(argc > 1){
        code = atoi(argv[1]);
    }
    fflush(NULL);
    exit(code);
}

static int b_export(int argc, char **argv){
    if(argc == 1){
        vars_print(1);
        return 0;
    }
    int rc = 0;
    for(int i = 1; i < argc; i++){
        char *eq = strchr(argv[i], '=');
        size_t n = eq ? (size_t)(eq - argv[i]) : strlen(argv[i]);
        if(!valid_name(argv[i], n)){
            fprintf(stderr, "ysh: export: `%s': not a valid identifier\n", argv[i]);
            rc = 1;
            continue;
        }
        if(eq){
            char name[64];
            if(n >= sizeof(name)){
                rc = 1;
                continue;
            }
            memcpy(name, argv[i], n);
            name[n] = 0;
            var_set(name, eq + 1, 1);
        } else {
            var_mark_exported(argv[i]);
        }
    }
    return rc;
}

static int b_unset(int argc, char **argv){
    for(int i = 1; i < argc; i++){
        var_unset(argv[i]);
    }
    return 0;
}

static int b_history(int argc, char **argv){
    (void)argc;
    (void)argv;
    history_print();
    return 0;
}

static int b_clear(int argc, char **argv){
    (void)argc;
    (void)argv;
    fputs("\x1b[H\x1b[2J", stdout);
    fflush(stdout);
    return 0;
}

static int b_version(int argc, char **argv){
    (void)argc;
    (void)argv;
    printf("YOKAI OS version 0.1 (ysh %s)\n", YSH_VERSION);
    return 0;
}

static int b_help(int argc, char **argv){
    (void)argc;
    (void)argv;
    puts("ysh " YSH_VERSION " - the YokaiOS shell");
    puts("Built-ins : cd [dir|-]  pwd  exit [n]  export [NAME[=value]]  unset NAME  history  clear  version  help");
    puts("Syntax    : cmd args | cmd2   > file   >> file   < file   cmd &   a ; b   a && b   a || b");
    puts("Words     : 'literal'  \"with $VAR\"  \\x  $VAR  ${VAR}  $?  $$  ~  NAME=value");
    puts("Editing   : Left/Right Home/End Delete Backspace, Up/Down = history, Tab = complete paths/commands");
    puts("            Ctrl-A/E start/end, Ctrl-K/U kill to end/start, Ctrl-W kill word, Ctrl-L clear, Ctrl-C cancel, Ctrl-D exit");
    puts("Programs  : looked up in $PATH (ls cat echo mkdir rmdir rm mv env uptime date sleep ...)");
    return 0;
}

static const struct { const char *name; builtin_fn fn; } table[] = {
    {"cd", b_cd}, {"pwd", b_pwd}, {"exit", b_exit}, {"export", b_export}, {"unset", b_unset},
    {"history", b_history}, {"clear", b_clear}, {"version", b_version}, {"help", b_help},
};
#define NBUILTINS ((int)(sizeof(table) / sizeof(table[0])))

int builtin_lookup(const char *name){
    for(int i = 0; i < NBUILTINS; i++){
        if(strcmp(table[i].name, name) == 0){
            return i;
        }
    }
    return -1;
}

int builtin_run(int idx, int argc, char **argv){
    return table[idx].fn(argc, argv);
}

const char *builtin_name(int idx){
    return table[idx].name;
}

int builtin_count(void){
    return NBUILTINS;
}

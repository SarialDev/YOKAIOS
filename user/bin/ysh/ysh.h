/* ysh - shell-ul YokaiOS (userspace). Fisiere: main.c (bucla, prompt), line.c (editare, istoric, Tab),
 * parse.c (tokeni, expandare), vars.c (variabile), exec.c (pipe, redirectari, procese), builtins.c */
#ifndef YSH_H
#define YSH_H

#include <stddef.h>
#include <sys/types.h>

#define YSH_VERSION "0.1"
#define MAX_CMDS 16         /* comenzi intr-un pipeline */
#define MAX_WORDS 30        /* cuvinte pe comanda (kernelul accepta cel mult 32 argumente la execve) */
#define MAX_REDIRS 4
#define MAX_ITEMS 16        /* pipeline-uri pe linie */
#define LINE_CAP 1024

/* --- vars.c --- */
void vars_init(void);
const char *var_get(const char *name);
/* exported: 1 = exporta, 0 = nu, -1 = pastreaza starea (variabila noua = neexportata) */
int var_set(const char *name, const char *value, int exported);
void var_unset(const char *name);
void var_mark_exported(const char *name);
/* mediu pentru execve: NULL-terminat, de eliberat cu envp_free */
char **vars_envp(void);
void envp_free(char **envp);
void vars_print(int only_exported);
int valid_name(const char *s, size_t n);
extern int last_status;

/* --- parse.c --- */
enum { R_IN, R_OUT, R_APPEND };
enum { SEP_END, SEP_SEMI, SEP_BG, SEP_AND, SEP_OR };

struct redir { int op; char *path; };          /* path: cuvant brut (neexpandat) */
struct cmd {
    int nwords;
    char *words[MAX_WORDS];                    /* cuvinte brute, cu ghilimele; se expandeaza la executie */
    int nredirs;
    struct redir redirs[MAX_REDIRS];
};
struct pipeline {
    int ncmds;
    struct cmd cmds[MAX_CMDS];
    int sep;                                   /* ce urmeaza dupa pipeline: SEP_* */
};
struct list {
    int n;
    struct pipeline items[MAX_ITEMS];
};

/* 0 = ok, -1 = eroare (mesajul in err) */
int parse_line(const char *line, struct list *out, char *err, size_t errcap);
void list_free(struct list *l);
/* Expandeaza un cuvant brut: ghilimele, \, $VAR ${VAR} $? $$, ~. Intoarce NULL daca cuvantul dispare (expansiune
 * goala fara ghilimele) sau la memorie insuficienta (*dropped = 0) */
char *expand_word(const char *raw, int *dropped);

/* --- exec.c --- */
int run_list(struct list *l);
void reap_jobs(void);
int jobs_count(void);
extern const char *shell_name;

/* --- builtins.c --- */
int builtin_lookup(const char *name);              /* indice >= 0 sau -1 */
int builtin_run(int idx, int argc, char **argv);
const char *builtin_name(int idx);
int builtin_count(void);

/* --- line.c --- */
/* Citeste o linie cu editare. Intoarce lungimea (0 = linie goala), -1 la EOF (Ctrl-D pe linie goala) */
int line_read(const char *prompt, char *out, size_t cap);
void history_add(const char *line);
void history_print(void);
int history_count(void);

/* --- main.c --- */
int execute_line(const char *line);
void prompt_build(char *buf, size_t cap);

#endif

/* Executia: fork + execve, pipe-uri, redirectari, procese in fundal (&), && si || */
#include "ysh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

const char *shell_name = "ysh";

#define MAX_JOBS 16

struct job {
    int id;
    pid_t pid;
    char text[48];
};
static struct job jobs[MAX_JOBS];
static int next_job_id = 1;

/* Comanda expandata */
struct xcmd {
    char *argv[MAX_WORDS + 1];
    int argc;
    char *env_assign[MAX_WORDS];        /* VAR=val din fata comenzii */
    int nassign;
    int fd_in, fd_out;                  /* -1 = neschimbat */
};

static void xcmd_free(struct xcmd *x){
    for(int i = 0; i < x->argc; i++){
        free(x->argv[i]);
    }
    for(int i = 0; i < x->nassign; i++){
        free(x->env_assign[i]);
    }
    x->argc = x->nassign = 0;
}

static int is_assignment(const char *raw){
    const char *eq = strchr(raw, '=');
    return eq && eq != raw && valid_name(raw, (size_t)(eq - raw));
}

/* Expandeaza cuvintele; 0 sau -1 */
static int expand_cmd(const struct cmd *c, struct xcmd *x){
    memset(x, 0, sizeof(*x));
    x->fd_in = x->fd_out = -1;
    int i = 0;
    /* Atribuiri in fata comenzii (A=1 B=2 cmd): doar cat timp urmeaza alte cuvinte sau toate sunt atribuiri */
    for(; i < c->nwords && is_assignment(c->words[i]); i++){
        int dropped;
        char *w = expand_word(c->words[i], &dropped);
        if(!w){
            xcmd_free(x);
            return -1;
        }
        x->env_assign[x->nassign++] = w;
    }
    for(; i < c->nwords; i++){
        int dropped;
        char *w = expand_word(c->words[i], &dropped);
        if(!w){
            if(dropped){
                continue;
            }
            xcmd_free(x);
            return -1;
        }
        x->argv[x->argc++] = w;
    }
    x->argv[x->argc] = NULL;
    return 0;
}

/* Deschide redirectarile in ordine (ultima castiga); mesaje de eroare incluse. 0 sau -1 */
static int open_redirs(const struct cmd *c, struct xcmd *x){
    for(int i = 0; i < c->nredirs; i++){
        int dropped;
        char *path = expand_word(c->redirs[i].path, &dropped);
        if(!path || !*path){
            fprintf(stderr, "ysh: ambiguous redirect\n");
            free(path);
            return -1;
        }
        int op = c->redirs[i].op;
        int fd = op == R_IN ? open(path, O_RDONLY)
               : open(path, O_WRONLY | O_CREAT | (op == R_APPEND ? O_APPEND : O_TRUNC), 0644);
        if(fd < 0){
            fprintf(stderr, "ysh: %s: %s\n", path, strerror(errno));
            free(path);
            return -1;
        }
        free(path);
        int *slot = op == R_IN ? &x->fd_in : &x->fd_out;
        if(*slot >= 0){
            close(*slot);
        }
        *slot = fd;
    }
    return 0;
}

/* execve cu cautare in PATH; revine doar la eroare, cu errno si "gasit" (alt errno decat ENOENT la vreo cale) */
static void exec_search(char **argv, char **envp){
    const char *name = argv[0];
    if(strchr(name, '/')){
        execve(name, argv, envp);
        return;
    }
    const char *path = var_get("PATH");
    if(!path){
        path = "/bin:/sbin";
    }
    int saved = ENOENT;
    char buf[256];
    while(1){
        size_t n = strcspn(path, ":");
        if(n + strlen(name) + 2 <= sizeof(buf)){
            memcpy(buf, path, n);
            buf[n] = '/';
            strcpy(buf + n + 1, name);
            execve(buf, argv, envp);
            if(errno != ENOENT){
                saved = errno;
            }
        }
        if(!path[n]){
            break;
        }
        path += n + 1;
    }
    errno = saved;
}

static void child_exec(struct xcmd *x, char **envp) __attribute__((noreturn));
static void child_exec(struct xcmd *x, char **envp){
    if(x->argc == 0){
        _exit(0);
    }
    int b = builtin_lookup(x->argv[0]);
    if(b >= 0){
        int st = builtin_run(b, x->argc, x->argv);
        fflush(NULL);
        _exit(st);
    }
    // Atribuirile din fata comenzii (A=1 cmd) intra in mediul copilului: mediul exportat + atribuirile
    char **use = envp;
    if(x->nassign){
        size_t base = 0;
        while(envp && envp[base]){
            base++;
        }
        char **own = calloc(base + (size_t)x->nassign + 1, sizeof(char *));
        if(own){
            size_t k = 0;
            for(size_t i = 0; i < base; i++){
                size_t nl = strcspn(envp[i], "=");
                int replaced = 0;
                for(int a = 0; a < x->nassign; a++){
                    if(strncmp(x->env_assign[a], envp[i], nl) == 0 && x->env_assign[a][nl] == '='){
                        replaced = 1;
                    }
                }
                if(!replaced){
                    own[k++] = envp[i];
                }
            }
            for(int a = 0; a < x->nassign; a++){
                own[k++] = x->env_assign[a];
            }
            own[k] = NULL;
            use = own;
        }
    }
    exec_search(x->argv, use);
    int e = errno;
    if(e == ENOENT){
        fprintf(stderr, "ysh: %s: command not found\n", x->argv[0]);
        fflush(NULL);
        _exit(127);
    }
    if(e == ENOEXEC || e == EISDIR){
        fprintf(stderr, "ysh: %s: cannot execute\n", x->argv[0]);
    } else {
        fprintf(stderr, "ysh: %s: %s\n", x->argv[0], strerror(e));
    }
    fflush(NULL);
    _exit(126);
}

static int status_code(int st, pid_t pid){
    if(WIFEXITED(st)){
        return WEXITSTATUS(st);
    }
    fprintf(stderr, "ysh: process %d killed (signal %d)\n", (int)pid, WTERMSIG(st));
    return 128 + WTERMSIG(st);
}

static void job_add(pid_t pid, const char *text){
    for(int i = 0; i < MAX_JOBS; i++){
        if(jobs[i].pid == 0){
            jobs[i].id = next_job_id++;
            jobs[i].pid = pid;
            snprintf(jobs[i].text, sizeof(jobs[i].text), "%s", text);
            printf("[%d] %d\n", jobs[i].id, (int)pid);
            return;
        }
    }
    printf("[?] %d\n", (int)pid);
}

int jobs_count(void){
    int n = 0;
    for(int i = 0; i < MAX_JOBS; i++){
        n += jobs[i].pid != 0;
    }
    return n;
}

/* Aduna copiii terminati din fundal, fara sa blocheze */
void reap_jobs(void){
    for(;;){
        int st;
        pid_t p = waitpid(-1, &st, WNOHANG);
        if(p <= 0){
            break;
        }
        for(int i = 0; i < MAX_JOBS; i++){
            if(jobs[i].pid == p){
                printf("[%d]+ Done    %s\n", jobs[i].id, jobs[i].text);
                jobs[i].pid = 0;
                break;
            }
        }
    }
    if(jobs_count() == 0){
        next_job_id = 1;
    }
}

/* Comanda simpla in shell (builtin sau atribuiri), cu redirectarile aplicate temporar. -1 = nu e cazul */
static int run_in_shell(const struct cmd *c, struct xcmd *x){
    if(x->argc == 0){
        // doar atribuiri (sau doar redirectari, care deschid/creeaza fisierele)
        if(open_redirs(c, x) < 0){
            return 1;
        }
        if(x->fd_in >= 0){
            close(x->fd_in);
        }
        if(x->fd_out >= 0){
            close(x->fd_out);
        }
        for(int i = 0; i < x->nassign; i++){
            char *eq = strchr(x->env_assign[i], '=');
            *eq = 0;
            var_set(x->env_assign[i], eq + 1, -1);
            *eq = '=';
        }
        return 0;
    }
    int b = builtin_lookup(x->argv[0]);
    if(b < 0){
        return -1;
    }
    if(open_redirs(c, x) < 0){
        return 1;
    }
    int save_in = -1, save_out = -1;
    fflush(NULL);
    if(x->fd_in >= 0){
        save_in = dup(0);
        dup2(x->fd_in, 0);
        close(x->fd_in);
    }
    if(x->fd_out >= 0){
        save_out = dup(1);
        dup2(x->fd_out, 1);
        close(x->fd_out);
    }
    int st = builtin_run(b, x->argc, x->argv);
    fflush(NULL);
    if(save_in >= 0){
        dup2(save_in, 0);
        close(save_in);
    }
    if(save_out >= 0){
        dup2(save_out, 1);
        close(save_out);
    }
    return st;
}

static int run_pipeline(struct pipeline *pl, int background){
    struct xcmd x[MAX_CMDS];
    int n = pl->ncmds;
    int ok = 1;
    for(int i = 0; i < n; i++){
        if(expand_cmd(&pl->cmds[i], &x[i]) < 0){
            fprintf(stderr, "ysh: out of memory\n");
            for(int k = 0; k < i; k++){
                xcmd_free(&x[k]);
            }
            return 1;
        }
    }
    int status = 0;
    if(n == 1 && !background){
        int r = run_in_shell(&pl->cmds[0], &x[0]);
        if(r >= 0){
            xcmd_free(&x[0]);
            return r;
        }
    }

    char **envp = vars_envp();
    pid_t pids[MAX_CMDS];
    int npids = 0;
    int prev_read = -1;
    fflush(NULL);
    for(int i = 0; i < n && ok; i++){
        int pfd[2] = {-1, -1};
        if(i + 1 < n && pipe(pfd) < 0){
            fprintf(stderr, "ysh: pipe: %s\n", strerror(errno));
            ok = 0;
            break;
        }
        // Redirectarile se deschid in copil (erorile apar acolo); aici doar pentru ca fisierele sa nu ramana deschise in parinte
        pid_t pid = fork();
        if(pid < 0){
            fprintf(stderr, "ysh: fork: %s\n", strerror(errno));
            if(pfd[0] >= 0){
                close(pfd[0]);
                close(pfd[1]);
            }
            ok = 0;
            break;
        }
        if(pid == 0){
            if(prev_read >= 0){
                dup2(prev_read, 0);
            } else if(background){
                int nul = open("/dev/null", O_RDONLY);
                if(nul >= 0){
                    dup2(nul, 0);
                    close(nul);
                }
            }
            if(pfd[1] >= 0){
                dup2(pfd[1], 1);
            }
            if(prev_read >= 0){
                close(prev_read);
            }
            if(pfd[0] >= 0){
                close(pfd[0]);
                close(pfd[1]);
            }
            if(open_redirs(&pl->cmds[i], &x[i]) < 0){
                _exit(1);
            }
            if(x[i].fd_in >= 0){
                dup2(x[i].fd_in, 0);
                close(x[i].fd_in);
            }
            if(x[i].fd_out >= 0){
                dup2(x[i].fd_out, 1);
                close(x[i].fd_out);
            }
            child_exec(&x[i], envp);
        }
        pids[npids++] = pid;
        if(prev_read >= 0){
            close(prev_read);
        }
        if(pfd[1] >= 0){
            close(pfd[1]);
        }
        prev_read = pfd[0];
    }
    if(prev_read >= 0){
        close(prev_read);
    }
    envp_free(envp);

    if(background){
        char text[48];
        snprintf(text, sizeof(text), "%s", x[0].argc ? x[0].argv[0] : "");
        if(npids){
            job_add(pids[npids - 1], text);
        }
        // Celelalte procese din pipeline le aduna reap_jobs (nu au job propriu)
    } else {
        for(int i = 0; i < npids; i++){
            int st = 0;
            pid_t r;
            do {
                r = waitpid(pids[i], &st, 0);
            } while(r < 0 && errno == EINTR);
            if(i == npids - 1){
                status = r < 0 ? 1 : status_code(st, pids[i]);
            }
        }
        if(!ok){
            status = 1;
        }
    }
    for(int i = 0; i < n; i++){
        xcmd_free(&x[i]);
    }
    return ok ? status : 1;
}

int run_list(struct list *l){
    int prev_sep = SEP_END;
    int status = last_status;
    for(int i = 0; i < l->n; i++){
        struct pipeline *pl = &l->items[i];
        int run = 1;
        if(prev_sep == SEP_AND && last_status != 0){
            run = 0;
        } else if(prev_sep == SEP_OR && last_status == 0){
            run = 0;
        }
        if(run){
            status = run_pipeline(pl, pl->sep == SEP_BG);
            last_status = status;
        }
        prev_sep = pl->sep;
    }
    return status;
}

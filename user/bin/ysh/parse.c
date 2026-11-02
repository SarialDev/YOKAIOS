/* Impartirea liniei in pipeline-uri si cuvinte (cu ghilimele pastrate) si expandarea unui cuvant la executie.
 * Gramatica: lista = pipeline ((';' | '&' | '&&' | '||') pipeline)* ; pipeline = comanda ('|' comanda)* ;
 * comanda = (cuvant | ('<' | '>' | '>>') cuvant)+ ; '#' la inceput de cuvant = comentariu */
#include "ysh.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

static int is_op_char(char c){
    return c == '|' || c == ';' || c == '&' || c == '<' || c == '>';
}

static void seterr(char *err, size_t cap, const char *msg, const char *arg){
    snprintf(err, cap, arg ? "%s `%s'" : "%s", msg, arg);
}

/* Sfarsitul cuvantului care incepe la p (respecta '...', "..." si \) ; NULL = ghilimele neinchise */
static const char *word_end(const char *p){
    while(*p){
        char c = *p;
        if(c == '\\'){
            p += p[1] ? 2 : 1;
        } else if(c == '\''){
            p++;
            while(*p && *p != '\''){
                p++;
            }
            if(!*p){
                return NULL;
            }
            p++;
        } else if(c == '"'){
            p++;
            while(*p && *p != '"'){
                p += (*p == '\\' && p[1]) ? 2 : 1;
            }
            if(!*p){
                return NULL;
            }
            p++;
        } else if(isspace((unsigned char)c) || is_op_char(c)){
            break;
        } else {
            p++;
        }
    }
    return p;
}

void list_free(struct list *l){
    for(int i = 0; i < l->n; i++){
        struct pipeline *pl = &l->items[i];
        for(int c = 0; c < pl->ncmds; c++){
            struct cmd *cm = &pl->cmds[c];
            for(int w = 0; w < cm->nwords; w++){
                free(cm->words[w]);
            }
            for(int r = 0; r < cm->nredirs; r++){
                free(cm->redirs[r].path);
            }
        }
    }
    l->n = 0;
}

static int cmd_empty(const struct cmd *c){
    return c->nwords == 0 && c->nredirs == 0;
}

int parse_line(const char *line, struct list *out, char *err, size_t errcap){
    memset(out, 0, sizeof(*out));
    struct pipeline *pl = NULL;          /* pipeline-ul in lucru (out->items[out->n]) */
    struct cmd *cur = NULL;
    int pending = -1;                    /* operator de redirectare asteptand un cuvant */
    const char *p = line;

    #define FAIL(msg, arg) do { seterr(err, errcap, msg, arg); list_free(out); return -1; } while(0)

    /* Incepe un pipeline nou daca nu exista unul */
    #define ENSURE() do { \
        if(!pl){ \
            if(out->n == MAX_ITEMS) FAIL("syntax error: too many commands on one line", NULL); \
            pl = &out->items[out->n]; pl->ncmds = 1; cur = &pl->cmds[0]; \
        } \
    } while(0)

    for(;;){
        while(*p && isspace((unsigned char)*p)){
            p++;
        }
        if(!*p){
            break;
        }
        if(*p == '#'){
            break;
        }
        if(is_op_char(*p)){
            char c = *p;
            char c2 = p[1];
            if(pending >= 0){
                FAIL("syntax error near unexpected token", c == '|' ? "|" : c == ';' ? ";" : c == '&' ? "&" : c == '<' ? "<" : ">");
            }
            if(c == '<' || c == '>'){
                ENSURE();
                if(cur->nredirs == MAX_REDIRS){
                    FAIL("syntax error: too many redirections", NULL);
                }
                if(c == '<'){
                    pending = R_IN;
                    p++;
                } else if(c2 == '>'){
                    pending = R_APPEND;
                    p += 2;
                } else {
                    pending = R_OUT;
                    p++;
                }
                continue;
            }
            if(c == '|' && c2 != '|'){
                if(!pl || cmd_empty(cur)){
                    FAIL("syntax error near unexpected token", "|");
                }
                if(pl->ncmds == MAX_CMDS){
                    FAIL("syntax error: pipeline too long", NULL);
                }
                cur = &pl->cmds[pl->ncmds++];
                p++;
                continue;
            }
            int sep;
            if(c == ';'){
                sep = SEP_SEMI;
                p++;
            } else if(c == '&' && c2 == '&'){
                sep = SEP_AND;
                p += 2;
            } else if(c == '|'){
                sep = SEP_OR;
                p += 2;
            } else {
                sep = SEP_BG;
                p++;
            }
            if(!pl || cmd_empty(cur)){
                /* ';' sau '&' fara comanda: ';' la inceput se ignora, restul sunt erori */
                if(sep == SEP_SEMI && !pl){
                    continue;
                }
                FAIL("syntax error near unexpected token", sep == SEP_SEMI ? ";" : sep == SEP_BG ? "&" : sep == SEP_AND ? "&&" : "||");
            }
            pl->sep = sep;
            out->n++;
            pl = NULL;
            cur = NULL;
            continue;
        }
        const char *end = word_end(p);
        if(!end){
            FAIL("syntax error: unterminated quote", NULL);
        }
        char *w = strndup(p, (size_t)(end - p));
        if(!w){
            FAIL("out of memory", NULL);
        }
        p = end;
        ENSURE();
        if(pending >= 0){
            cur->redirs[cur->nredirs].op = pending;
            cur->redirs[cur->nredirs].path = w;
            cur->nredirs++;
            pending = -1;
        } else {
            if(cur->nwords == MAX_WORDS){
                free(w);
                FAIL("syntax error: too many arguments", NULL);
            }
            cur->words[cur->nwords++] = w;
        }
    }
    if(pending >= 0){
        FAIL("syntax error near unexpected token", "newline");
    }
    if(pl){
        if(cmd_empty(cur)){
            FAIL("syntax error near unexpected token", "newline");
        }
        pl->sep = SEP_END;
        out->n++;
    }
    return 0;
    #undef FAIL
    #undef ENSURE
}

/* ---- expandare ---- */

struct sbuf {
    char *p;
    size_t len, cap;
    int fail;
};

static void sb_put(struct sbuf *b, char c){
    if(b->fail){
        return;
    }
    if(b->len + 2 > b->cap){
        size_t ncap = b->cap ? b->cap * 2 : 64;
        char *np = realloc(b->p, ncap);
        if(!np){
            b->fail = 1;
            return;
        }
        b->p = np;
        b->cap = ncap;
    }
    b->p[b->len++] = c;
    b->p[b->len] = 0;
}

static void sb_str(struct sbuf *b, const char *s){
    for(; s && *s; s++){
        sb_put(b, *s);
    }
}

/* $... la p (p arata dupa '$'); intoarce pozitia de dupa expansiune, sau p daca '$' ramane literal */
static const char *expand_var(const char *p, struct sbuf *b, int *expanded){
    if(*p == '?'){
        char num[16];
        snprintf(num, sizeof(num), "%d", last_status);
        sb_str(b, num);
        *expanded = 1;
        return p + 1;
    }
    if(*p == '$'){
        char num[16];
        snprintf(num, sizeof(num), "%d", (int)getpid());
        sb_str(b, num);
        *expanded = 1;
        return p + 1;
    }
    const char *name = p;
    const char *after;
    size_t n;
    if(*p == '{'){
        const char *close = strchr(p, '}');
        if(!close || !valid_name(p + 1, (size_t)(close - p - 1))){
            return NULL;
        }
        name = p + 1;
        n = (size_t)(close - p - 1);
        after = close + 1;
    } else {
        n = 0;
        while(isalnum((unsigned char)p[n]) || p[n] == '_'){
            n++;
        }
        if(!valid_name(p, n)){
            return NULL;
        }
        after = p + n;
    }
    char key[64];
    if(n >= sizeof(key)){
        n = sizeof(key) - 1;
    }
    memcpy(key, name, n);
    key[n] = 0;
    sb_str(b, var_get(key));
    *expanded = 1;
    return after;
}

char *expand_word(const char *raw, int *dropped){
    struct sbuf b = {NULL, 0, 0, 0};
    int quoted = 0, expanded = 0;
    const char *p = raw;
    if(dropped){
        *dropped = 0;
    }
    if(p[0] == '~' && (p[1] == 0 || p[1] == '/')){
        const char *home = var_get("HOME");
        if(home){
            sb_str(&b, home);
            p++;
        }
    }
    while(*p){
        char c = *p;
        if(c == '\\'){
            quoted = 1;
            if(p[1]){
                sb_put(&b, p[1]);
                p += 2;
            } else {
                p++;
            }
        } else if(c == '\''){
            quoted = 1;
            p++;
            while(*p && *p != '\''){
                sb_put(&b, *p++);
            }
            if(*p){
                p++;
            }
        } else if(c == '"'){
            quoted = 1;
            p++;
            while(*p && *p != '"'){
                if(*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$' || p[1] == '`')){
                    sb_put(&b, p[1]);
                    p += 2;
                } else if(*p == '$'){
                    const char *next = expand_var(p + 1, &b, &expanded);
                    if(next){
                        p = next;
                    } else {
                        sb_put(&b, *p++);
                    }
                } else {
                    sb_put(&b, *p++);
                }
            }
            if(*p){
                p++;
            }
        } else if(c == '$'){
            const char *next = expand_var(p + 1, &b, &expanded);
            if(next){
                p = next;
            } else {
                sb_put(&b, *p++);
            }
        } else {
            sb_put(&b, *p++);
        }
    }
    if(b.fail){
        free(b.p);
        return NULL;
    }
    if(!b.p){
        b.p = strdup("");
        if(!b.p){
            return NULL;
        }
    }
    if(b.len == 0 && !quoted && expanded){
        free(b.p);
        if(dropped){
            *dropped = 1;
        }
        return NULL;
    }
    return b.p;
}

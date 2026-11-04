#include "yedit.h"
#include <stdlib.h>
#include <string.h>

#define GAP_MIN 4096

void gap_init(struct gap *g){
    g->buf = NULL;
    g->cap = g->gs = g->ge = 0;
}

size_t gap_len(const struct gap *g){
    return g->cap - (g->ge - g->gs);
}

char gap_at(const struct gap *g, size_t i){
    return g->buf[i < g->gs ? i : i + (g->ge - g->gs)];
}

/* Muta golul astfel incat sa inceapa la pos */
static void gap_move(struct gap *g, size_t pos){
    if(pos < g->gs){
        size_t n = g->gs - pos;
        memmove(g->buf + g->ge - n, g->buf + pos, n);
        g->gs -= n;
        g->ge -= n;
    } else if(pos > g->gs){
        size_t n = pos - g->gs;
        memmove(g->buf + g->gs, g->buf + g->ge, n);
        g->gs += n;
        g->ge += n;
    }
}

static int gap_reserve(struct gap *g, size_t need){
    if(g->ge - g->gs >= need){
        return 0;
    }
    size_t len = gap_len(g);
    gap_move(g, len);                           /* golul la capat, textul in [0, len) */
    size_t extra = len / 2 > GAP_MIN ? len / 2 : GAP_MIN;
    size_t ncap = len + need + extra;
    char *nb = realloc(g->buf, ncap);
    if(!nb){
        return -1;
    }
    g->buf = nb;
    g->cap = ncap;
    g->gs = len;
    g->ge = ncap;
    return 0;
}

int gap_insert(struct gap *g, size_t pos, const char *s, size_t n){
    if(pos > gap_len(g)){
        pos = gap_len(g);
    }
    if(gap_reserve(g, n) < 0){
        return -1;
    }
    gap_move(g, pos);
    memcpy(g->buf + g->gs, s, n);
    g->gs += n;
    return 0;
}

void gap_delete(struct gap *g, size_t pos, size_t n){
    size_t len = gap_len(g);
    if(pos >= len){
        return;
    }
    if(n > len - pos){
        n = len - pos;
    }
    gap_move(g, pos);
    g->ge += n;
}

void gap_copy(const struct gap *g, size_t pos, size_t n, char *dst){
    for(size_t i = 0; i < n; i++){
        dst[i] = gap_at(g, pos + i);
    }
}

size_t gap_bol(const struct gap *g, size_t pos){
    while(pos > 0 && gap_at(g, pos - 1) != '\n'){
        pos--;
    }
    return pos;
}

size_t gap_eol(const struct gap *g, size_t pos){
    size_t len = gap_len(g);
    while(pos < len && gap_at(g, pos) != '\n'){
        pos++;
    }
    return pos;
}

size_t gap_lines_before(const struct gap *g, size_t pos){
    size_t n = 0;
    for(size_t i = 0; i < pos; i++){
        if(gap_at(g, i) == '\n'){
            n++;
        }
    }
    return n;
}

size_t gap_line_offset(const struct gap *g, size_t n){
    size_t len = gap_len(g);
    size_t p = 0;
    while(n > 0){
        size_t e = gap_eol(g, p);
        if(e >= len){
            break;
        }
        p = e + 1;
        n--;
    }
    return p;
}

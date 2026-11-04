/* yedit: editor de text in terminal, in stilul nano. Fisiere: gap.c (buffer cu gol), term.c (terminal brut, taste, iesire cu buffer),
 * main.c (editorul: ecran, comenzi, cautare, salvare) */
#ifndef YEDIT_H
#define YEDIT_H
#include <stddef.h>

/* ---------------- gap.c: textul e un singur sir cu un gol (gap) in jurul cursorului ---------------- */
struct gap {
    char *buf;
    size_t cap;         /* dimensiunea alocata */
    size_t gs, ge;      /* golul: [gs, ge) */
};

void gap_init(struct gap *g);
size_t gap_len(const struct gap *g);
char gap_at(const struct gap *g, size_t i);
/* 0 = ok, -1 = memorie insuficienta */
int gap_insert(struct gap *g, size_t pos, const char *s, size_t n);
void gap_delete(struct gap *g, size_t pos, size_t n);
void gap_copy(const struct gap *g, size_t pos, size_t n, char *dst);
/* Inceputul si sfarsitul (pozitia lui '\n' sau len) liniei care contine pos */
size_t gap_bol(const struct gap *g, size_t pos);
size_t gap_eol(const struct gap *g, size_t pos);
/* Numarul de '\n' in [0, pos) = indicele (de la 0) al liniei lui pos */
size_t gap_lines_before(const struct gap *g, size_t pos);
/* Inceputul liniei n (de la 0); daca sunt mai putine linii, inceputul ultimei */
size_t gap_line_offset(const struct gap *g, size_t n);

/* ---------------- term.c ---------------- */
enum { K_EOF = -1, K_UP = 1000, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_ESC, K_IGNORE };

/* Mod brut daca stdin e terminal; intoarce 1 daca e terminal (altfel taste citite dintr-un fisier/pipe, folosit de teste) */
int term_open(void);
void term_close(void);
void term_size(int *rows, int *cols);
/* Octet (0..255), K_* sau K_EOF */
int term_key(void);
void out_put(char c);
void out_bytes(const char *s, size_t n);
void out_str(const char *s);
void out_flush(void);

#endif

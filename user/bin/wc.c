/* wc [-l] [-w] [-c] [fisier...]: linii, cuvinte, octeti (fara optiuni: toate trei) */
#include <stdio.h>
#include <string.h>
#include <errno.h>

static int opt_l, opt_w, opt_c;

static void count(FILE *f, unsigned long *lines, unsigned long *words, unsigned long *bytes){
    int c, in_word = 0;
    *lines = *words = *bytes = 0;
    while((c = getc(f)) != EOF){
        (*bytes)++;
        if(c == '\n'){
            (*lines)++;
        }
        if(c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\v' || c == '\f'){
            in_word = 0;
        } else if(!in_word){
            in_word = 1;
            (*words)++;
        }
    }
}

static void show(unsigned long l, unsigned long w, unsigned long b, const char *name){
    if(opt_l) printf("%7lu", l);
    if(opt_w) printf("%7lu", w);
    if(opt_c) printf("%7lu", b);
    if(name){
        printf(" %s", name);
    }
    putchar('\n');
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: wc [-l] [-w] [-c] [fisier...]");
        return 0;
    }
    int i = 1;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++){
        for(const char *o = argv[i] + 1; *o; o++){
            if(*o == 'l') opt_l = 1;
            else if(*o == 'w') opt_w = 1;
            else if(*o == 'c') opt_c = 1;
            else {
                fprintf(stderr, "wc: optiune necunoscuta -%c\n", *o);
                return 2;
            }
        }
    }
    if(!(opt_l || opt_w || opt_c)){
        opt_l = opt_w = opt_c = 1;
    }
    unsigned long l, w, b;
    if(i >= argc){
        count(stdin, &l, &w, &b);
        show(l, w, b, NULL);
        return 0;
    }
    int rc = 0, files = 0;
    unsigned long tl = 0, tw = 0, tb = 0;
    for(; i < argc; i++){
        FILE *f = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
        if(!f){
            fprintf(stderr, "wc: %s: %s\n", argv[i], strerror(errno));
            rc = 1;
            continue;
        }
        count(f, &l, &w, &b);
        if(f != stdin){
            fclose(f);
        }
        show(l, w, b, argv[i]);
        tl += l; tw += w; tb += b;
        files++;
    }
    if(files > 1){
        show(tl, tw, tb, "total");
    }
    return rc;
}

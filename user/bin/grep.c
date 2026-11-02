/* grep [-i] [-v] [-n] [-c] text [fisier...]: liniile care contin textul (substring simplu, nu regex) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

static int opt_i, opt_v, opt_n, opt_c;

static int contains(const char *line, const char *pat){
    if(!opt_i){
        return strstr(line, pat) != NULL;
    }
    size_t n = strlen(pat);
    for(; *line; line++){
        if(strncasecmp(line, pat, n) == 0){
            return 1;
        }
    }
    return 0;
}

// Numarul de linii potrivite; -1 la eroare
static long search(FILE *f, const char *pat, const char *name){
    char *line = NULL;
    size_t cap = 0;
    long matched = 0, lineno = 0;
    long len;
    while((len = getline(&line, &cap, f)) >= 0){
        lineno++;
        int hit = contains(line, pat) != opt_v;
        if(!hit){
            continue;
        }
        matched++;
        if(opt_c){
            continue;
        }
        if(name){
            printf("%s:", name);
        }
        if(opt_n){
            printf("%ld:", lineno);
        }
        fputs(line, stdout);
        if(len == 0 || line[len - 1] != '\n'){
            putchar('\n');
        }
    }
    free(line);
    return matched;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: grep [-i] [-v] [-n] [-c] text [fisier...]");
        return 0;
    }
    int i = 1;
    for(; i < argc && argv[i][0] == '-' && argv[i][1]; i++){
        for(const char *o = argv[i] + 1; *o; o++){
            if(*o == 'i') opt_i = 1;
            else if(*o == 'v') opt_v = 1;
            else if(*o == 'n') opt_n = 1;
            else if(*o == 'c') opt_c = 1;
            else {
                fprintf(stderr, "grep: optiune necunoscuta -%c\n", *o);
                return 2;
            }
        }
    }
    if(i >= argc){
        fprintf(stderr, "uz: grep [-i] [-v] [-n] [-c] text [fisier...]\n");
        return 2;
    }
    const char *pat = argv[i++];
    long total = 0;
    int err = 0;
    if(i >= argc){
        long m = search(stdin, pat, NULL);
        if(opt_c){
            printf("%ld\n", m);
        }
        return m > 0 ? 0 : 1;
    }
    int many = argc - i > 1;
    for(; i < argc; i++){
        FILE *f = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");
        if(!f){
            fprintf(stderr, "grep: %s: %s\n", argv[i], strerror(errno));
            err = 1;
            continue;
        }
        long m = search(f, pat, many ? argv[i] : NULL);
        if(f != stdin){
            fclose(f);
        }
        if(opt_c){
            if(many){
                printf("%s:", argv[i]);
            }
            printf("%ld\n", m);
        }
        total += m;
    }
    return err ? 2 : total > 0 ? 0 : 1;
}

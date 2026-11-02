/* tail [-n N] [fisier...]: ultimele N linii (implicit 10); fara fisier citeste stdin */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int tail_stream(FILE *f, long n){
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if(!buf){
        return 1;
    }
    int c;
    while((c = getc(f)) != EOF){
        if(len == cap){
            cap *= 2;
            char *nb = realloc(buf, cap);
            if(!nb){
                free(buf);
                return 1;
            }
            buf = nb;
        }
        buf[len++] = (char)c;
    }
    // Pozitia de start: dupa al (n+1)-lea '\n' numarat de la sfarsit (un '\n' final nu conteaza)
    size_t start = len;
    long seen = 0;
    if(n > 0){
        size_t i = len;
        if(i > 0 && buf[i - 1] == '\n'){
            i--;
        }
        start = 0;
        while(i > 0){
            if(buf[--i] == '\n' && ++seen == n){
                start = i + 1;
                break;
            }
        }
    }
    fwrite(buf + start, 1, len - start, stdout);
    free(buf);
    return 0;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: tail [-n N] [fisier...]");
        return 0;
    }
    long n = 10;
    int i = 1;
    if(i < argc && argv[i][0] == '-' && argv[i][1]){
        const char *v;
        if(strcmp(argv[i], "-n") == 0 && i + 1 < argc){
            v = argv[++i];
        } else if(argv[i][1] == 'n'){
            v = argv[i] + 2;
        } else {
            v = argv[i] + 1;
        }
        char *end;
        n = strtol(v, &end, 10);
        if(!*v || *end || n < 0){
            fprintf(stderr, "tail: numar de linii invalid: %s\n", v);
            return 2;
        }
        i++;
    }
    if(i >= argc){
        return tail_stream(stdin, n);
    }
    int rc = 0;
    int many = argc - i > 1;
    for(int k = i; k < argc; k++){
        FILE *f = strcmp(argv[k], "-") == 0 ? stdin : fopen(argv[k], "r");
        if(!f){
            fprintf(stderr, "tail: %s: %s\n", argv[k], strerror(errno));
            rc = 1;
            continue;
        }
        if(many){
            printf("%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        }
        rc |= tail_stream(f, n);
        if(f != stdin){
            fclose(f);
        }
    }
    return rc;
}

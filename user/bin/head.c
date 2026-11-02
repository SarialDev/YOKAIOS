/* head [-n N] [fisier...]: primele N linii (implicit 10); fara fisier citeste stdin */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int head_stream(FILE *f, long n){
    int c;
    while(n > 0 && (c = getc(f)) != EOF){
        putchar(c);
        if(c == '\n'){
            n--;
        }
    }
    return 0;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: head [-n N] [fisier...]");
        return 0;
    }
    long n = 10;
    int i = 1;
    if(i < argc && argv[i][0] == '-' && argv[i][1]){
        const char *v = NULL;
        if(strcmp(argv[i], "-n") == 0 && i + 1 < argc){
            v = argv[++i];
        } else if(argv[i][1] == 'n'){
            v = argv[i] + 2;
        } else {
            v = argv[i] + 1;        // -N
        }
        char *end;
        n = strtol(v, &end, 10);
        if(!*v || *end || n < 0){
            fprintf(stderr, "head: numar de linii invalid: %s\n", v);
            return 2;
        }
        i++;
    }
    if(i >= argc){
        return head_stream(stdin, n);
    }
    int rc = 0;
    int many = argc - i > 1;
    for(int k = i; k < argc; k++){
        FILE *f = strcmp(argv[k], "-") == 0 ? stdin : fopen(argv[k], "r");
        if(!f){
            fprintf(stderr, "head: %s: %s\n", argv[k], strerror(errno));
            rc = 1;
            continue;
        }
        if(many){
            printf("%s==> %s <==\n", k > i ? "\n" : "", argv[k]);
        }
        head_stream(f, n);
        if(f != stdin){
            fclose(f);
        }
    }
    return rc;
}

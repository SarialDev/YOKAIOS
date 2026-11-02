/* echo [-n] [text...] */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: echo [-n] [text...]");
        return 0;
    }
    int i = 1;
    int newline = 1;
    if(i < argc && strcmp(argv[i], "-n") == 0){
        newline = 0;
        i++;
    }
    for(; i < argc; i++){
        fputs(argv[i], stdout);
        if(i + 1 < argc){
            putchar(' ');
        }
    }
    if(newline){
        putchar('\n');
    }
    return 0;
}

/* Pachetul de proba hello.yokai: tipareste un salut si argumentele */
#include <stdio.h>

int main(int argc, char **argv){
    printf("salut de la hello 1.0.0 (pachet .yokai), argc=%d\n", argc);
    for(int i = 1; i < argc; i++){
        printf("  arg %d = \"%s\"\n", i, argv[i]);
    }
    return 0;
}

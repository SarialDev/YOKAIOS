/* env: tipareste mediul procesului (VAR=valoare, cate una pe linie) */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: env");
        return 0;
    }
    for(char **e = environ; e && *e; e++){
        puts(*e);
    }
    return 0;
}

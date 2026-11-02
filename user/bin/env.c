/* env: tipareste mediul procesului (VAR=valoare, cate una pe linie) */
#include <stdio.h>
#include <unistd.h>

int main(void){
    for(char **e = environ; e && *e; e++){
        puts(*e);
    }
    return 0;
}

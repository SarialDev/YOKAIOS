/* sleep secunde */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: sleep secunde");
        return 0;
    }
    if(argc != 2){
        fprintf(stderr, "uz: sleep secunde\n");
        return 2;
    }
    sleep((unsigned)atoi(argv[1]));
    return 0;
}

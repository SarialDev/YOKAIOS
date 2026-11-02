/* sleep secunde */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv){
    if(argc != 2){
        fprintf(stderr, "uz: sleep secunde\n");
        return 2;
    }
    sleep((unsigned)atoi(argv[1]));
    return 0;
}

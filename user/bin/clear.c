/* clear: sterge ecranul (ANSI: ESC[2J ESC[H) */
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: clear");
        return 0;
    }
    if(argc != 1){
        fprintf(stderr, "uz: clear\n");
        return 2;
    }
    static const char seq[] = "\033[2J\033[H";
    return write(1, seq, sizeof(seq) - 1) < 0 ? 1 : 0;
}

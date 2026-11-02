/* Pachetul de proba hello.yokai: tipareste un salut si argumentele; "--caps" tipareste capabilitatile (permitted effective, hex) */
#include <stdio.h>
#include <string.h>
#include <sys/capability.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--caps") == 0){
        struct cap_state st;
        if(capget(0, &st) < 0){
            return 1;
        }
        printf("caps: permitted=%llx effective=%llx\n", st.permitted, st.effective);
        return 0;
    }
    printf("salut de la hello 1.0.0 (pachet .yokai), argc=%d\n", argc);
    for(int i = 1; i < argc; i++){
        printf("  arg %d = \"%s\"\n", i, argv[i]);
    }
    return 0;
}

/* uptime: "up <secunde> s" din /proc/uptime */
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: uptime");
        return 0;
    }
    FILE *f = fopen("/proc/uptime", "r");
    char buf[64];
    if(!f || !fgets(buf, sizeof(buf), f)){
        fprintf(stderr, "uptime: /proc/uptime indisponibil\n");
        return 1;
    }
    fclose(f);
    int n = 0;
    while(buf[n] && buf[n] != ' ' && buf[n] != '\n'){
        n++;
    }
    printf("up %.*s s\n", n, buf);
    return 0;
}

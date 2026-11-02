/* uptime: "up <secunde> s" din /proc/uptime */
#include <stdio.h>

int main(void){
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

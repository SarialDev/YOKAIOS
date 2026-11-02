/* free [-k|-m]: memoria din /proc/meminfo (implicit KiB) */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned long field(const char *name){
    FILE *f = fopen("/proc/meminfo", "r");
    if(!f){
        return 0;
    }
    char line[128];
    unsigned long v = 0;
    size_t n = strlen(name);
    while(fgets(line, sizeof(line), f)){
        if(strncmp(line, name, n) == 0 && line[n] == ':'){
            v = strtoul(line + n + 1, NULL, 10);
            break;
        }
    }
    fclose(f);
    return v;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: free [-k|-m]");
        return 0;
    }
    unsigned long div = 1;
    const char *unit = "KiB";
    for(int i = 1; i < argc; i++){
        if(strcmp(argv[i], "-k") == 0){
            div = 1;
            unit = "KiB";
        } else if(strcmp(argv[i], "-m") == 0){
            div = 1024;
            unit = "MiB";
        } else {
            fprintf(stderr, "uz: free [-k|-m]\n");
            return 2;
        }
    }
    unsigned long total = field("MemTotal");
    if(!total){
        fprintf(stderr, "free: /proc/meminfo indisponibil\n");
        return 1;
    }
    unsigned long freek = field("MemFree"), avail = field("MemAvailable");
    unsigned long stot = field("SwapTotal"), sfree = field("SwapFree");
    printf("%-6s %12s %12s %12s %12s\n", unit, "total", "used", "free", "available");
    printf("Mem:   %12lu %12lu %12lu %12lu\n", total / div, (total - freek) / div, freek / div, avail / div);
    printf("Swap:  %12lu %12lu %12lu\n", stot / div, (stot - sfree) / div, sfree / div);
    return 0;
}

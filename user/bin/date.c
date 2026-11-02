/* date: ora curenta UTC */
#include <stdio.h>
#include <string.h>
#include <time.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: date");
        return 0;
    }
    time_t t = time(NULL);
    struct tm tm;
    char buf[64];
    gmtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    puts(buf);
    return 0;
}

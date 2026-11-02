/* date: ora curenta UTC */
#include <stdio.h>
#include <time.h>

int main(void){
    time_t t = time(NULL);
    struct tm tm;
    char buf[64];
    gmtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    puts(buf);
    return 0;
}

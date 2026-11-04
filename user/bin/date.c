/* date: ora curenta UTC; `date -s "AAAA-LL-ZZ OO:MM:SS"` o schimba (cere CAP_SYS_ADMIN) */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: date [-s \"AAAA-LL-ZZ OO:MM:SS\"]");
        return 0;
    }
    if(argc == 3 && strcmp(argv[1], "-s") == 0){
        struct tm tm;
        int y, mo, d, h, mi, s = 0;
        if(sscanf(argv[2], "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) < 5 || mo < 1 || mo > 12 || d < 1 || d > 31 ||
           h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60){
            fprintf(stderr, "date: format invalid\n");
            return 2;
        }
        memset(&tm, 0, sizeof(tm));
        tm.tm_year = y - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = d;
        tm.tm_hour = h;
        tm.tm_min = mi;
        tm.tm_sec = s;
        struct timeval tv = {timegm(&tm), 0};
        if(settimeofday(&tv, NULL) < 0){
            perror("date");
            return 1;
        }
    } else if(argc != 1){
        fprintf(stderr, "uz: date [-s \"AAAA-LL-ZZ OO:MM:SS\"]\n");
        return 2;
    }
    time_t t = time(NULL);
    struct tm tm;
    char buf[64];
    gmtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
    puts(buf);
    return 0;
}

#include <time.h>
#include <stdio.h>
#include <string.h>

clock_t clock(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000L + ts.tv_nsec / 1000;
}

double difftime(time_t a, time_t b){
    return (double)(a - b);
}

static const int mdays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static int leap(long y){
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

struct tm *gmtime_r(const time_t *tp, struct tm *tm){
    long t = *tp;
    long days = t / 86400, rem = t % 86400;
    if(rem < 0){
        rem += 86400;
        days--;
    }
    tm->tm_hour = (int)(rem / 3600);
    tm->tm_min = (int)(rem % 3600 / 60);
    tm->tm_sec = (int)(rem % 60);
    tm->tm_wday = (int)((days % 7 + 11) % 7);       // 1970-01-01 a fost joi
    long y = 1970;
    while(days < 0){
        y--;
        days += leap(y) ? 366 : 365;
    }
    while(days >= (leap(y) ? 366 : 365)){
        days -= leap(y) ? 366 : 365;
        y++;
    }
    tm->tm_year = (int)(y - 1900);
    tm->tm_yday = (int)days;
    int m = 0;
    while(m < 11 && days >= mdays[m] + (m == 1 && leap(y))){
        days -= mdays[m] + (m == 1 && leap(y));
        m++;
    }
    tm->tm_mon = m;
    tm->tm_mday = (int)days + 1;
    tm->tm_isdst = 0;
    return tm;
}

static struct tm tm_static;

struct tm *gmtime(const time_t *t){ return gmtime_r(t, &tm_static); }
struct tm *localtime_r(const time_t *t, struct tm *tm){ return gmtime_r(t, tm); }
struct tm *localtime(const time_t *t){ return gmtime_r(t, &tm_static); }

time_t timegm(struct tm *tm){
    long y = tm->tm_year + 1900L + tm->tm_mon / 12;
    int mon = tm->tm_mon % 12;
    if(mon < 0){
        mon += 12;
        y--;
    }
    long days = 0;
    for(long i = 1970; i < y; i++){
        days += leap(i) ? 366 : 365;
    }
    for(long i = y; i < 1970; i++){
        days -= leap(i) ? 366 : 365;
    }
    for(int m = 0; m < mon; m++){
        days += mdays[m] + (m == 1 && leap(y));
    }
    days += tm->tm_mday - 1;
    time_t t = days * 86400L + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec;
    gmtime_r(&t, tm);                                // normalizeaza campurile
    return t;
}

time_t mktime(struct tm *tm){ return timegm(tm); }

static const char *const wdays[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *const months[12] = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};

// saptamana ISO 8601: an si numar
static void iso_week(const struct tm *tm, int *year, int *week){
    int wd = (tm->tm_wday + 6) % 7;                 // luni = 0
    int y = tm->tm_year + 1900;
    int w = (tm->tm_yday - wd + 10) / 7;
    if(w < 1){
        y--;
        int dec31_yday = leap(y) ? 365 : 364;
        int dec31_wd = ((tm->tm_wday - tm->tm_yday - 1) % 7 + 14) % 7;   // ziua saptamanii a lui 31 dec anterior
        dec31_wd = (dec31_wd + 6) % 7;
        w = (dec31_yday - dec31_wd + 10) / 7;
    } else if(w == 53){
        int days_in_year = leap(y) ? 366 : 365;
        if(tm->tm_yday - wd + 3 >= days_in_year){
            w = 1;
            y++;
        }
    }
    *year = y;
    *week = w;
}

size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm){
    size_t n = 0;
    char tmp[64];
    for(; *fmt; fmt++){
        const char *src = tmp;
        if(*fmt != '%'){
            tmp[0] = *fmt;
            tmp[1] = 0;
        } else {
            switch(*++fmt){
                case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
                case 'y': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_year % 100); break;
                case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
                case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
                case 'e': snprintf(tmp, sizeof(tmp), "%2d", tm->tm_mday); break;
                case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
                case 'I': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12); break;
                case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
                case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
                case 'j': snprintf(tmp, sizeof(tmp), "%03d", tm->tm_yday + 1); break;
                case 'p': snprintf(tmp, sizeof(tmp), "%s", tm->tm_hour >= 12 ? "PM" : "AM"); break;
                case 'a': snprintf(tmp, sizeof(tmp), "%.3s", wdays[tm->tm_wday % 7]); break;
                case 'A': snprintf(tmp, sizeof(tmp), "%s", wdays[tm->tm_wday % 7]); break;
                case 'b': case 'h': snprintf(tmp, sizeof(tmp), "%.3s", months[tm->tm_mon % 12]); break;
                case 'B': snprintf(tmp, sizeof(tmp), "%s", months[tm->tm_mon % 12]); break;
                case 'F': snprintf(tmp, sizeof(tmp), "%d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
                case 'T': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
                case 'D': snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
                case 'C': snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_year + 1900) / 100); break;
                case 'u': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday ? tm->tm_wday : 7); break;
                case 'w': snprintf(tmp, sizeof(tmp), "%d", tm->tm_wday); break;
                case 'R': snprintf(tmp, sizeof(tmp), "%02d:%02d", tm->tm_hour, tm->tm_min); break;
                case 'r': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d %s", tm->tm_hour % 12 ? tm->tm_hour % 12 : 12,
                                   tm->tm_min, tm->tm_sec, tm->tm_hour >= 12 ? "PM" : "AM"); break;
                case 'X': snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
                case 'x': snprintf(tmp, sizeof(tmp), "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, tm->tm_year % 100); break;
                case 'c': snprintf(tmp, sizeof(tmp), "%.3s %.3s %2d %02d:%02d:%02d %d", wdays[tm->tm_wday % 7],
                                   months[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
                                   tm->tm_year + 1900); break;
                case 'U': snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_yday + 7 - tm->tm_wday) / 7); break;
                case 'W': snprintf(tmp, sizeof(tmp), "%02d", (tm->tm_yday + 7 - (tm->tm_wday + 6) % 7) / 7); break;
                case 'V': { int y, w; iso_week(tm, &y, &w); snprintf(tmp, sizeof(tmp), "%02d", w); break; }
                case 'G': { int y, w; iso_week(tm, &y, &w); snprintf(tmp, sizeof(tmp), "%d", y); break; }
                case 'g': { int y, w; iso_week(tm, &y, &w); snprintf(tmp, sizeof(tmp), "%02d", y % 100); break; }
                case 'Z': snprintf(tmp, sizeof(tmp), "UTC"); break;
                case 'z': snprintf(tmp, sizeof(tmp), "+0000"); break;
                case 'n': snprintf(tmp, sizeof(tmp), "\n"); break;
                case 't': snprintf(tmp, sizeof(tmp), "\t"); break;
                case '%': snprintf(tmp, sizeof(tmp), "%%"); break;
                case 0: return 0;
                default: snprintf(tmp, sizeof(tmp), "%%%c", *fmt); break;
            }
        }
        size_t l = strlen(src);
        if(n + l + 1 > max){
            return 0;
        }
        memcpy(s + n, src, l);
        n += l;
    }
    if(n + 1 > max){
        return 0;
    }
    s[n] = 0;
    return n;
}

char *asctime(const struct tm *tm){
    static char buf[32];
    snprintf(buf, sizeof(buf), "%.3s %.3s %2d %02d:%02d:%02d %d\n", wdays[tm->tm_wday % 7], months[tm->tm_mon % 12],
             tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec, tm->tm_year + 1900);
    return buf;
}

char *ctime(const time_t *t){
    return asctime(gmtime(t));
}

#ifndef _TIME_H
#define _TIME_H
#include <stddef.h>
typedef long time_t;
typedef long clock_t;
typedef int clockid_t;
#define CLOCKS_PER_SEC 1000000L
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
struct timespec { time_t tv_sec; long tv_nsec; };
struct tm {
    int tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
};
time_t time(time_t *t);
int clock_gettime(clockid_t clk, struct timespec *ts);
int nanosleep(const struct timespec *req, struct timespec *rem);
clock_t clock(void);
double difftime(time_t a, time_t b);
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t mktime(struct tm *tm);
time_t timegm(struct tm *tm);
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm);
char *asctime(const struct tm *tm);
char *ctime(const time_t *t);
#endif

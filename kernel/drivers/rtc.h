#pragma once
#include "types.h"

struct RtcTime {
    uint16_t year;      // 1970..9999
    uint8_t month;      // 1..12
    uint8_t day;        // 1..31
    uint8_t hour;       // 0..23
    uint8_t minute;
    uint8_t second;
};

// Valorile brute ale registrelor CMOS (inainte de decodare BCD/12h)
struct RtcRaw {
    uint8_t sec, min, hour, day, mon, year, century, status_b;
};

// Decodare pura (testabila): BCD sau binar dupa bitul DM din status B, 12h/24h dupa bitul 24H
// (in 12h bitul 7 al orei = PM). century = 0 daca registrul nu exista
RtcTime rtc_decode(const RtcRaw& raw);
// Secunde de la 1970-01-01 00:00:00 UTC; calendar gregorian
uint64_t rtc_to_epoch(const RtcTime& t);
RtcTime rtc_from_epoch(uint64_t epoch);

// Citeste CMOS-ul: asteapta sfarsitul update-in-progress si repeta citirea pana doua citiri consecutive
// coincid. RTC-ul se presupune ca tine UTC (ca in QEMU)
RtcTime rtc_read();
// Citeste RTC-ul o data si fixeaza epoca la boot = epoca RTC - uptime; dupa clock_init
void rtc_init();
// Ora reala UTC: epoca boot + uptime
uint64_t wall_time();
uint64_t wall_time_ms();
// "YYYY-MM-DD HH:MM:SS UTC" in buf (min. 24 octeti)
void rtc_format(const RtcTime& t, char* buf, int cap);

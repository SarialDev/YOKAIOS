#include "tests/test_rtc.h"
#include "drivers/rtc.h"
#include "lib/kprintf.h"
#include "core/panic.h"

namespace {

#define CHECK(cond) do { if(!(cond)) panic("test_rtc: %s (linia %d)", #cond, __LINE__); } while(0)

const uint8_t B_24H = 0x02, B_BIN = 0x04;

void check_decode(){
    // BCD, 24h: 2026-10-29 17:45:09
    RtcRaw r = {0x09, 0x45, 0x17, 0x29, 0x10, 0x26, 0x20, B_24H};
    RtcTime t = rtc_decode(r);
    CHECK(t.year == 2026 && t.month == 10 && t.day == 29 && t.hour == 17 && t.minute == 45 && t.second == 9);

    // Binar, 24h
    RtcRaw b = {9, 45, 17, 29, 10, 26, 20, (uint8_t)(B_24H | B_BIN)};
    t = rtc_decode(b);
    CHECK(t.year == 2026 && t.month == 10 && t.day == 29 && t.hour == 17 && t.minute == 45 && t.second == 9);

    // BCD, 12h: 05:45 PM = 17:45; 12:10 AM = 00:10; 12:10 PM = 12:10; 11:59 AM = 11:59
    RtcRaw p = {0x00, 0x45, 0x85, 0x01, 0x01, 0x30, 0x20, 0};
    CHECK(rtc_decode(p).hour == 17);
    p.hour = 0x12;
    CHECK(rtc_decode(p).hour == 0);
    p.hour = 0x92;
    CHECK(rtc_decode(p).hour == 12);
    p.hour = 0x11;
    CHECK(rtc_decode(p).hour == 11);
    // Binar, 12h: 3 PM
    RtcRaw q = {0, 0, (uint8_t)(0x80 | 3), 1, 1, 30, 20, B_BIN};
    CHECK(rtc_decode(q).hour == 15);

    // Fara registru de secol: 20xx
    RtcRaw c = {0, 0, 0, 1, 1, 0x05, 0, B_24H};
    CHECK(rtc_decode(c).year == 2005);
}

void check_epoch(){
    RtcTime t = {1970, 1, 1, 0, 0, 0};
    CHECK(rtc_to_epoch(t) == 0);
    RtcTime y2k = {2000, 1, 1, 0, 0, 0};
    CHECK(rtc_to_epoch(y2k) == 946684800ULL);
    RtcTime a = {2026, 10, 29, 12, 11, 36};
    CHECK(rtc_to_epoch(a) == 1793275896ULL);
    RtcTime leap = {2024, 2, 29, 23, 59, 59};
    CHECK(rtc_to_epoch(leap) == 1709251199ULL);
    RtcTime far = {2100, 3, 1, 0, 0, 0};    // 2100 nu e bisect
    CHECK(rtc_to_epoch(far) == 4107542400ULL);

    // Round-trip pe un interval larg
    for(uint64_t e = 0; e < 4200000000ULL; e += 86399ULL * 13 + 1234){
        RtcTime r = rtc_from_epoch(e);
        CHECK(rtc_to_epoch(r) == e);
    }
    RtcTime r = rtc_from_epoch(1709251199ULL);
    CHECK(r.year == 2024 && r.month == 2 && r.day == 29 && r.hour == 23 && r.second == 59);

    char buf[32];
    rtc_format(a, buf, sizeof(buf));
    const char* want = "2026-10-29 12:11:36 UTC";
    for(int i = 0; i < 24; i++){
        CHECK(buf[i] == want[i]);
    }
}

void check_live(){
    RtcTime t = rtc_read();
    CHECK(t.year >= 2000 && t.month >= 1 && t.month <= 12 && t.day >= 1 && t.day <= 31);
    CHECK(t.hour < 24 && t.minute < 60 && t.second < 60);
    uint64_t w = wall_time();
    uint64_t e = rtc_to_epoch(t);
    CHECK(w + 2 >= e && w <= e + 2);
}

}

void test_rtc_run(){
    check_decode();
    kprintf("[OK] test_rtc decode\n");
    check_epoch();
    kprintf("[OK] test_rtc epoch\n");
    check_live();
    kprintf("[OK] test_rtc\n");
}

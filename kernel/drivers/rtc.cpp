#include "drivers/rtc.h"
#include "io.h"
#include "time/clock.h"
#include "sync/spinlock.h"
#include "lib/kprintf.h"

namespace {

const uint16_t CMOS_INDEX = 0x70;
const uint16_t CMOS_DATA = 0x71;
const uint8_t REG_SEC = 0x00, REG_MIN = 0x02, REG_HOUR = 0x04, REG_DAY = 0x07, REG_MON = 0x08, REG_YEAR = 0x09;
const uint8_t REG_STATUS_A = 0x0A, REG_STATUS_B = 0x0B, REG_CENTURY = 0x32;
const uint8_t UIP = 0x80;           // status A: update in progress
const uint8_t B_24H = 0x02, B_BIN = 0x04;

Spinlock cmos_lock("cmos");
uint64_t boot_epoch_ms = 0;
bool rtc_ok = false;

uint8_t cmos_read(uint8_t reg){
    outb(CMOS_INDEX, reg);          // bitul 7 = 0: NMI ramane activ
    return inb(CMOS_DATA);
}

uint8_t bcd(uint8_t v){
    return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
}


// Algoritmul "days from civil" (H. Hinnant)
int64_t days_from_civil(int64_t y, unsigned m, unsigned d){
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

void snapshot(RtcRaw& r){
    while(cmos_read(REG_STATUS_A) & UIP){
        __asm__ volatile("pause");
    }
    r.sec = cmos_read(REG_SEC);
    r.min = cmos_read(REG_MIN);
    r.hour = cmos_read(REG_HOUR);
    r.day = cmos_read(REG_DAY);
    r.mon = cmos_read(REG_MON);
    r.year = cmos_read(REG_YEAR);
    r.century = cmos_read(REG_CENTURY);
    r.status_b = cmos_read(REG_STATUS_B);
}

bool same(const RtcRaw& a, const RtcRaw& b){
    return a.sec == b.sec && a.min == b.min && a.hour == b.hour && a.day == b.day && a.mon == b.mon &&
           a.year == b.year && a.century == b.century;
}

}

RtcTime rtc_decode(const RtcRaw& raw){
    bool binary = raw.status_b & B_BIN;
    bool h24 = raw.status_b & B_24H;
    auto conv = [&](uint8_t v){ return binary ? v : bcd(v); };

    RtcTime t;
    t.second = conv(raw.sec);
    t.minute = conv(raw.min);
    bool pm = !h24 && (raw.hour & 0x80);
    uint8_t h = conv(raw.hour & 0x7F);
    if(!h24){
        h %= 12;                    // 12 AM = 0, 12 PM = 12
        if(pm){
            h += 12;
        }
    }
    t.hour = h;
    t.day = conv(raw.day);
    t.month = conv(raw.mon);
    unsigned yy = conv(raw.year);
    unsigned cc = raw.century ? conv(raw.century) : 0;
    // Registrul de secol lipseste sau e nesigur: presupunem 20xx
    if(cc < 19 || cc > 99){
        cc = 20;
    }
    t.year = (uint16_t)(cc * 100 + yy);
    return t;
}

uint64_t rtc_to_epoch(const RtcTime& t){
    int64_t days = days_from_civil(t.year, t.month, t.day);
    return (uint64_t)days * 86400 + t.hour * 3600u + t.minute * 60u + t.second;
}

RtcTime rtc_from_epoch(uint64_t epoch){
    uint64_t days = epoch / 86400;
    uint32_t rem = (uint32_t)(epoch % 86400);
    RtcTime t;
    t.hour = (uint8_t)(rem / 3600);
    t.minute = (uint8_t)(rem % 3600 / 60);
    t.second = (uint8_t)(rem % 60);
    // civil from days (Hinnant)
    int64_t z = (int64_t)days + 719468;
    int64_t era = z / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    t.day = (uint8_t)(doy - (153 * mp + 2) / 5 + 1);
    t.month = (uint8_t)(mp < 10 ? mp + 3 : mp - 9);
    t.year = (uint16_t)(y + (t.month <= 2));
    return t;
}

RtcTime rtc_read(){
    RtcRaw a, b;
    SpinlockGuard g(cmos_lock);
    snapshot(a);
    for(int i = 0; i < 10; i++){
        snapshot(b);
        if(same(a, b)){
            break;
        }
        a = b;
    }
    return rtc_decode(a);
}

void rtc_init(){
    RtcTime t = rtc_read();
    uint64_t up = uptime_ms();
    boot_epoch_ms = rtc_to_epoch(t) * 1000 - up;
    rtc_ok = true;
    char buf[32];
    rtc_format(t, buf, sizeof(buf));
    kprintf("rtc: %s\n", buf);
}

uint64_t wall_time_ms(){
    return rtc_ok ? boot_epoch_ms + uptime_ms() : 0;
}

uint64_t wall_time(){
    return wall_time_ms() / 1000;
}

void rtc_format(const RtcTime& t, char* buf, int cap){
    // YYYY-MM-DD HH:MM:SS UTC = 23 caractere + NUL
    if(cap < 24){
        if(cap > 0){
            buf[0] = '\0';
        }
        return;
    }
    int n = 0;
    auto num = [&](unsigned v, int digits){
        for(int i = digits - 1; i >= 0; i--){
            buf[n + i] = '0' + v % 10;
            v /= 10;
        }
        n += digits;
    };
    auto ch = [&](char c){ buf[n++] = c; };
    num(t.year, 4); ch('-'); num(t.month, 2); ch('-'); num(t.day, 2); ch(' ');
    num(t.hour, 2); ch(':'); num(t.minute, 2); ch(':'); num(t.second, 2);
    const char* s = " UTC";
    while(*s){
        ch(*s++);
    }
    buf[n] = '\0';
}

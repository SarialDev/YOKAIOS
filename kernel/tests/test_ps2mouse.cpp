#include "tests/test_ps2mouse.h"
#include "drivers/ps2mouse.h"
#include "hal/hal.h"
#include "lib/kprintf.h"
#include "core/panic.h"

namespace {

#define CHECK(cond) do { if(!(cond)) panic("test_ps2mouse: %s (linia %d)", #cond, __LINE__); } while(0)

// Trimite octetii si intoarce cate pachete complete au rezultat; ultimul ramane in `out`
int feed_all(Ps2MouseDecoder& d, const uint8_t* bytes, int n, MouseEvent& out){
    int packets = 0;
    for(int i = 0; i < n; i++){
        if(d.feed(bytes[i], out)){
            packets++;
        }
    }
    return packets;
}

void check_standard(){
    Ps2MouseDecoder d;
    MouseEvent m;
    // Stanga apasat, dx = +5, dy = +3 (sus la PS/2 => dy = -3 la noi)
    const uint8_t p1[] = {0x09, 5, 3};
    CHECK(feed_all(d, p1, 3, m) == 1);
    CHECK(m.dx == 5 && m.dy == -3 && m.dz == 0 && m.buttons == MOUSE_LEFT);

    // Semn: dx = -2 (0xFE cu bitul 4), dy = -10 (0xF6 cu bitul 5), dreapta + mijloc
    const uint8_t p2[] = {0x08 | 0x10 | 0x20 | 0x02 | 0x04, 0xFE, 0xF6};
    CHECK(feed_all(d, p2, 3, m) == 1);
    CHECK(m.dx == -2 && m.dy == 10 && m.buttons == (MOUSE_RIGHT | MOUSE_MIDDLE));

    // Extremele pe 9 biti
    const uint8_t p3[] = {0x08 | 0x10, 0x00, 0x00};
    CHECK(feed_all(d, p3, 3, m) == 1 && m.dx == -256 && m.dy == 0);
    const uint8_t p4[] = {0x08, 0xFF, 0xFF};
    CHECK(feed_all(d, p4, 3, m) == 1 && m.dx == 255 && m.dy == -255);

    // Doar butoanele s-au schimbat
    const uint8_t p5[] = {0x08, 0, 0};
    CHECK(feed_all(d, p5, 3, m) == 1 && m.dx == 0 && m.dy == 0 && m.buttons == 0);
}

void check_sync(){
    Ps2MouseDecoder d;
    MouseEvent m;
    // Octeti fara bitul 3 in pozitia 0 se arunca pana la primul octet valid
    const uint8_t noise[] = {0x00, 0x05, 0x01, 0x09, 7, 1};
    CHECK(feed_all(d, noise, 6, m) == 1);
    CHECK(m.dx == 7 && m.dy == -1 && m.buttons == MOUSE_LEFT);

    // Bitul 3 al unui octet de deplasare nu conteaza: pachetul 0x08, 0x08, 0x08 e valid
    const uint8_t p[] = {0x08, 0x08, 0x08};
    CHECK(feed_all(d, p, 3, m) == 1 && m.dx == 8 && m.dy == -8);

    // reset() arunca un pachet neterminat
    d.feed(0x09, m);
    d.feed(0x01, m);
    d.reset();
    const uint8_t q[] = {0x08, 1, 2};
    CHECK(feed_all(d, q, 3, m) == 1 && m.dx == 1 && m.dy == -2);
}

void check_overflow(){
    Ps2MouseDecoder d;
    MouseEvent m;
    const uint8_t ox[] = {0x08 | 0x40, 100, 100};
    const uint8_t oy[] = {0x08 | 0x80, 100, 100};
    CHECK(feed_all(d, ox, 3, m) == 0);
    CHECK(feed_all(d, oy, 3, m) == 0);
    // Dupa overflow decodorul ramane sincronizat
    const uint8_t ok[] = {0x08, 4, 4};
    CHECK(feed_all(d, ok, 3, m) == 1 && m.dx == 4 && m.dy == -4);
}

void check_wheel(){
    Ps2MouseDecoder d;
    d.set_wheel(true);
    MouseEvent m;
    const uint8_t p1[] = {0x0A, 1, 2, 0x01};
    CHECK(feed_all(d, p1, 4, m) == 1);
    CHECK(m.dx == 1 && m.dy == -2 && m.dz == 1 && m.buttons == MOUSE_RIGHT);
    const uint8_t p2[] = {0x08, 0, 0, 0xFF};
    CHECK(feed_all(d, p2, 4, m) == 1 && m.dz == -1);
    // Doua pachete lipite, cu un octet de zgomot intre ele
    const uint8_t two[] = {0x08, 1, 1, 2, 0x00, 0x08, 2, 2, 0xFE};
    CHECK(feed_all(d, two, 9, m) == 2 && m.dx == 2 && m.dz == -2);
}

void check_hal(){
    bool found = false;
    for(int i = 0; i < hal_count(HalKind::Input); i++){
        const char* n = hal_name(HalKind::Input, i);
        const char* want = "ps2-mouse";
        while(*want && *n == *want){
            n++;
            want++;
        }
        found = found || !*want;
    }
    CHECK(found);
}

}

void test_ps2mouse_run(){
    check_standard();
    check_sync();
    check_overflow();
    check_wheel();
    check_hal();
    kprintf("[OK] test_ps2mouse\n");
}

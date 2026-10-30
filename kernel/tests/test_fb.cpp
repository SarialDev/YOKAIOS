#include "tests/test_fb.h"
#include "drivers/fb/fb.h"
#include "drivers/fbcon.h"
#include "hal/console.h"
#include "hal/fb.h"
#include "lib/kprintf.h"
#include "core/panic.h"

namespace {

#define CHECK(cond) do { if(!(cond)) panic("test_fb: %s (linia %d)", #cond, __LINE__); } while(0)

// Pixelul din framebuffer-ul real (nu din back buffer)
uint32_t front_pixel(const FbInfo& i, int x, int y){
    return *(const uint32_t*)(i.addr + (uint64_t)y * i.pitch + (uint64_t)x * 4);
}

void check_primitives(){
    FbInfo info;
    Framebuffer* dev = hal_fb(0);
    CHECK(dev && dev->info(info));
    CHECK(info.width == fb_width() && info.height == fb_height() && info.bpp == 32);
    CHECK(info.width >= 640 && info.pitch >= info.width * 4);

    // Zona de lucru: coltul din dreapta-jos, in afara textului
    int bx = (int)fb_width() - 40;
    int by = (int)fb_height() - 40;
    uint32_t red = fb_rgb(255, 0, 0);
    uint32_t green = fb_rgb(0, 255, 0);
    uint32_t blue = fb_rgb(0, 0, 255);
    CHECK(red != green && green != blue && red != blue);
    CHECK(fb_color(0xFF0000) == red && fb_color(0x00FF00) == green);

    fb_fill_rect(bx, by, 40, 40, blue);
    CHECK(fb_getpixel(bx, by) == blue && fb_getpixel(bx + 39, by + 39) == blue);
    fb_putpixel(bx + 3, by + 4, red);
    CHECK(fb_getpixel(bx + 3, by + 4) == red && fb_getpixel(bx + 4, by + 4) == blue);
    // Framebuffer-ul real are aceleasi pixeli ca back buffer-ul
    CHECK(front_pixel(info, bx + 3, by + 4) == red && front_pixel(info, bx + 10, by + 10) == blue);

    // Decupare: nimic in afara ecranului, fara crash
    fb_putpixel(-1, 0, red);
    fb_putpixel((int)fb_width(), 0, red);
    fb_fill_rect(-50, -50, 30, 30, red);
    fb_fill_rect((int)fb_width() - 5, (int)fb_height() - 5, 100, 100, green);
    CHECK(fb_getpixel((int)fb_width() - 1, (int)fb_height() - 1) == green);
    CHECK(fb_getpixel(-1, -1) == 0 && fb_getpixel(0, (int)fb_height()) == 0);

    // blit 3x2, dintre care o parte iese din ecran
    uint32_t img[6] = {red, green, blue, blue, red, green};
    fb_blit(bx, by, img, 3, 2, 12);
    CHECK(fb_getpixel(bx, by) == red && fb_getpixel(bx + 1, by) == green && fb_getpixel(bx + 2, by + 1) == green);
    CHECK(front_pixel(info, bx + 2, by) == blue);
    fb_blit(-1, -1, img, 3, 2, 12);
    CHECK(fb_getpixel(0, 0) == fb_getpixel(0, 0));
    CHECK(fb_getpixel(0, 0) == red);      // img[4] = (1,1) ajunge in (0,0)
    fb_blit((int)fb_width() - 1, (int)fb_height() - 1, img, 3, 2, 12);
    CHECK(fb_getpixel((int)fb_width() - 1, (int)fb_height() - 1) == red);

    // Glif: bitul 7 = stanga
    const uint8_t rows[2] = {0x80, 0x01};
    fb_draw_glyph(bx + 8, by + 8, rows, 2, green, red);
    CHECK(fb_getpixel(bx + 8, by + 8) == green && fb_getpixel(bx + 9, by + 8) == red);
    CHECK(fb_getpixel(bx + 15, by + 9) == green && fb_getpixel(bx + 14, by + 9) == red);
    CHECK(front_pixel(info, bx + 8, by + 8) == green);
}

void check_scroll(){
    FbInfo info;
    CHECK(hal_fb(0)->info(info));
    int last = (int)fb_height() - 1;
    uint32_t red = fb_rgb(255, 0, 0);
    uint32_t blue = fb_rgb(0, 0, 255);
    fb_fill_rect(10, last, 4, 1, red);
    fb_fill_rect(10, last - 20, 4, 1, blue);
    fb_scroll_up(16, 0);
    CHECK(fb_getpixel(10, last - 16) == red);
    CHECK(fb_getpixel(10, last - 36) == blue);
    CHECK(fb_getpixel(10, last) == 0 && fb_getpixel(10, last - 15) == 0);
    CHECK(front_pixel(info, 10, last - 16) == red && front_pixel(info, 10, last) == 0);
    fb_flush();
    CHECK(front_pixel(info, 10, last - 36) == blue);
}

bool cell_has_color(int col, int row, uint32_t color){
    for(int y = 0; y < 16; y++){
        for(int x = 0; x < 8; x++){
            if(fb_getpixel(col * 8 + x, row * 16 + y) == color){
                return true;
            }
        }
    }
    return false;
}

void put_str(Console* con, const char* s){
    for(; *s; s++){
        con->putc(*s);
    }
}

void check_console(){
    Console* con = hal_console_screen();
    CHECK(con && fbcon_active() && con->cols() == (int)fb_width() / 8 && con->rows() == (int)fb_height() / 16);

    con->clear();
    int c, r;
    fbcon_log_pos(c, r);
    CHECK(c == 0 && r == 0);

    // Text alb implicit pe negru
    put_str(con, "Ab");
    CHECK((fbcon_cell(0, 0) & 0xFF) == 'A' && (fbcon_cell(0, 0) >> 8) == 0x0F);
    CHECK(cell_has_color(0, 0, fbcon_palette(15)));
    fbcon_log_pos(c, r);
    CHECK(c == 2 && r == 0);

    // ANSI: rosu aprins (bold + 31), apoi verde de fundal, apoi reset
    put_str(con, "\x1b[1;31mR\x1b[42mG\x1b[0mW");
    CHECK((fbcon_cell(2, 0) >> 8) == 0x0C && cell_has_color(2, 0, fbcon_palette(12)));
    CHECK((fbcon_cell(3, 0) >> 8) == 0x2C);
    CHECK((fbcon_cell(4, 0) >> 8) == 0x0F);
    // 90-97 si 100-107: variantele deschise
    put_str(con, "\x1b[94;103mX\x1b[mY");
    CHECK((fbcon_cell(5, 0) >> 8) == 0xE9 && (fbcon_cell(6, 0) >> 8) == 0x0F);

    // Pozitionare si stergere: ESC[3;5H muta pe linia 3, coloana 5 (de la 1)
    put_str(con, "\x1b[3;5Hz");
    CHECK((fbcon_cell(4, 2) & 0xFF) == 'z');
    put_str(con, "\x1b[2;1H12345\x1b[3D\x1b[K");
    CHECK((fbcon_cell(1, 1) & 0xFF) == '2' && (fbcon_cell(2, 1) & 0xFF) == ' ');
    put_str(con, "\x1b[2J");
    CHECK((fbcon_cell(0, 0) & 0xFF) == ' ' && (fbcon_cell(4, 2) & 0xFF) == ' ');

    // \n, \r, \b, \t
    con->clear();
    put_str(con, "ab\ncd\r\x1b[1Ce\bf\tg");
    CHECK((fbcon_cell(0, 1) & 0xFF) == 'c' && (fbcon_cell(1, 1) & 0xFF) == 'f' && (fbcon_cell(8, 1) & 0xFF) == 'g');

    // set_color (atribut VGA) si put_at
    con->set_color(0x4E);
    con->put_at("Q", 7, 5);
    CHECK((fbcon_cell(7, 5) & 0xFF) == 'Q' && (fbcon_cell(7, 5) >> 8) == 0x4E);
    con->set_color(0x0F);

    // Scroll: ultima linie plina -> un rand mai sus; ultima linie goala
    con->clear();
    con->put_at("S", 0, con->rows() - 1);
    for(int i = 0; i < con->rows() - 1; i++){
        con->putc('\n');
    }
    fbcon_log_pos(c, r);
    CHECK(r == con->rows() - 1);
    con->putc('\n');
    CHECK((fbcon_cell(0, con->rows() - 2) & 0xFF) == 'S' && (fbcon_cell(0, con->rows() - 1) & 0xFF) == ' ');
    CHECK(cell_has_color(0, con->rows() - 2, fbcon_palette(15)));
    CHECK(!cell_has_color(0, con->rows() - 1, fbcon_palette(15)));

    // Bloc CP437 (logo): celula plina
    con->clear();
    con->put_at("\xDB", 3, 3);
    CHECK(fb_getpixel(3 * 8, 3 * 16) == fbcon_palette(15) && fb_getpixel(3 * 8 + 7, 3 * 16 + 15) == fbcon_palette(15));
    con->clear();
}

}

void test_fb_run(){
    if(!fb_ready()){
        CHECK(!fbcon_active());
        CHECK(hal_console_screen());            // fb0 real lipseste (test_hal poate fi inregistrat un fake-fb inainte)
        kprintf("[OK] test_fb (fara framebuffer, VGA text)\n");
        return;
    }
    check_primitives();
    check_scroll();
    check_console();
    kprintf("[OK] test_fb\n");
}

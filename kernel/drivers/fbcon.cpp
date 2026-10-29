#include "drivers/fbcon.h"
#include "drivers/fb/fb.h"
#include "drivers/fb/font8x16.h"
#include "drivers/vga.h"
#include "hal/console.h"
#include "boot/multiboot2.h"
#include "lib/string.h"
#include "init.h"

namespace {

const int GLYPH_W = 8;
const int GLYPH_H = 16;
const int MAX_COLS = 256;
const int MAX_ROWS = 96;

const uint8_t DEFAULT_ATTR = 0x0F;      // alb pe negru
const uint8_t BG_DEFAULT = 0;
const uint8_t FG_DEFAULT = 15;

// Tema Dark, in ordinea culorilor VGA (0 negru, 1 albastru, 2 verde, 3 cyan, 4 rosu, 5 magenta, 6 galben-brun,
// 7 gri, 8..15 variantele deschise). Rosul (4 si 12) e culoarea de accent
const uint32_t PALETTE_RGB[16] = {
    0x000000, 0x3A5FCD, 0x2EA043, 0x2AA5B5, 0xD0202F, 0xA23CB4, 0xC89B1E, 0xC8C8C8,
    0x5C5C5C, 0x6C8CFF, 0x56D364, 0x4CD6E6, 0xFF3B47, 0xD670E8, 0xFFD84A, 0xFFFFFF,
};

// Ordinea ANSI (negru, rosu, verde, galben, albastru, magenta, cyan, alb) -> indice VGA
const uint8_t ANSI_TO_VGA[8] = {0, 4, 2, 6, 1, 5, 3, 7};

class FbConsole final : public Console {
public:
    constexpr FbConsole() = default;

    void setup(){
        ncols = (int)fb_width() / GLYPH_W;
        nrows = (int)fb_height() / GLYPH_H;
        if(ncols > MAX_COLS) ncols = MAX_COLS;
        if(nrows > MAX_ROWS) nrows = MAX_ROWS;
        for(int i = 0; i < 16; i++){
            pal[i] = fb_color(PALETTE_RGB[i]);
        }
        reset_attr();
        clear();
    }

    void clear() override {
        for(int r = 0; r < nrows; r++){
            for(int c = 0; c < ncols; c++){
                cells[r][c] = (uint16_t)(' ' | (attr << 8));
            }
        }
        fb_fill_rect(0, 0, (int)fb_width(), (int)fb_height(), pal[attr >> 4]);
        lx = ly = 0;
        cur_x = cur_y = -1;
    }

    void put_at(const char* text, int x, int y) override {
        if(x < 0 || y < 0){
            return;
        }
        int index = y * ncols + x;
        for(int i = 0; text[i] != '\0' && index < ncols * nrows; i++, index++){
            set_cell(index % ncols, index / ncols, (uint8_t)text[i]);
        }
    }

    void set_cursor(int x, int y) override {
        if(cur_x >= 0){
            draw_cell(cur_x, cur_y);
        }
        cur_x = x;
        cur_y = y;
        if(x < 0 || y < 0 || x >= ncols || y >= nrows){
            cur_x = cur_y = -1;
            return;
        }
        draw_cell(x, y);
        // Cursor: dunga rosie de 2 pixeli la baza celulei
        fb_fill_rect(x * GLYPH_W, (y + 1) * GLYPH_H - 2, GLYPH_W, 2, pal[12]);
    }

    void set_color(uint8_t c) override {
        fg = c & 15;
        bg = c >> 4;
        bold = false;
        apply_attr();
    }

    void putc(char ch) override {
        uint8_t c = (uint8_t)ch;
        if(esc == 1){
            if(c == '['){
                esc = 2;
                nparam = 0;
                for(int i = 0; i < MAX_PARAMS; i++){
                    params[i] = 0;
                }
            } else {
                esc = 0;
            }
            return;
        }
        if(esc == 2){
            csi_byte(c);
            return;
        }
        switch(c){
            case 0x1B:
                esc = 1;
                return;
            case '\n':
                lx = 0;
                newline();
                return;
            case '\r':
                lx = 0;
                return;
            case '\b':
                if(lx > 0){
                    lx--;
                }
                return;
            case '\t':
                lx = (lx + 8) & ~7;
                if(lx >= ncols){
                    lx = 0;
                    newline();
                }
                return;
            default:
                break;
        }
        if(lx >= ncols){
            lx = 0;
            newline();
        }
        set_cell(lx++, ly, c);
    }

    uint32_t flags() const override { return CONSOLE_SCREEN; }
    int cols() const override { return ncols; }
    int rows() const override { return nrows; }

    uint16_t cell(int c, int r) const { return cells[r][c]; }
    void log_pos(int& c, int& r) const { c = lx; r = ly; }
    uint32_t color(int idx) const { return pal[idx & 15]; }

private:
    static const int MAX_PARAMS = 4;

    void reset_attr(){
        fg = FG_DEFAULT;
        bg = BG_DEFAULT;
        bold = false;
        apply_attr();
    }

    void apply_attr(){
        uint8_t f = (bold && fg < 8) ? fg + 8 : fg;
        attr = (uint8_t)((bg << 4) | f);
    }

    void glyph_rows(uint8_t ch, uint8_t* out) const {
        // Blocuri CP437 pentru logo si cadre, sintetizate (fontul e Latin-15)
        if(ch == 0xDB || ch == 0xDC || ch == 0xDF){
            for(int i = 0; i < GLYPH_H; i++){
                bool on = ch == 0xDB || (ch == 0xDF ? i < GLYPH_H / 2 : i >= GLYPH_H / 2);
                out[i] = on ? 0xFF : 0x00;
            }
            return;
        }
        memcpy(out, font8x16[ch], GLYPH_H);
    }

    void draw_cell(int c, int r){
        uint16_t v = cells[r][c];
        uint8_t rows[GLYPH_H];
        glyph_rows((uint8_t)v, rows);
        fb_draw_glyph(c * GLYPH_W, r * GLYPH_H, rows, GLYPH_H, pal[(v >> 8) & 15], pal[v >> 12]);
    }

    void set_cell(int c, int r, uint8_t ch){
        cells[r][c] = (uint16_t)(ch | (attr << 8));
        draw_cell(c, r);
    }

    void newline(){
        if(++ly < nrows){
            return;
        }
        ly = nrows - 1;
        fb_scroll_up(GLYPH_H, pal[attr >> 4]);
        for(int r = 0; r < nrows - 1; r++){
            memcpy(cells[r], cells[r + 1], sizeof(uint16_t) * ncols);
        }
        for(int c = 0; c < ncols; c++){
            cells[nrows - 1][c] = (uint16_t)(' ' | (attr << 8));
        }
        // Cursorul desenat s-a mutat odata cu pixelii
        if(cur_y >= 0 && --cur_y < 0){
            cur_x = -1;
        }
    }

    void csi_byte(uint8_t c){
        if(c >= '0' && c <= '9'){
            if(nparam == 0){
                nparam = 1;
            }
            if(nparam <= MAX_PARAMS){
                int& p = params[nparam - 1];
                p = p * 10 + (c - '0');
            }
            return;
        }
        if(c == ';'){
            if(nparam == 0){
                nparam = 1;
            }
            if(nparam < MAX_PARAMS){
                nparam++;
            }
            return;
        }
        if(c >= 0x40 && c <= 0x7E){
            esc = 0;
            csi_final(c);
            return;
        }
        // Prefixe (?) si intermediari: ignorati
    }

    int param(int i, int dflt) const {
        return (i < nparam && params[i]) ? params[i] : dflt;
    }

    void csi_final(uint8_t c){
        switch(c){
            case 'm':
                sgr();
                break;
            case 'H':
            case 'f': {
                int r = param(0, 1) - 1;
                int cl = param(1, 1) - 1;
                ly = r < 0 ? 0 : (r >= nrows ? nrows - 1 : r);
                lx = cl < 0 ? 0 : (cl >= ncols ? ncols - 1 : cl);
                break;
            }
            case 'A': ly -= param(0, 1); if(ly < 0) ly = 0; break;
            case 'B': ly += param(0, 1); if(ly >= nrows) ly = nrows - 1; break;
            case 'C': lx += param(0, 1); if(lx >= ncols) lx = ncols - 1; break;
            case 'D': lx -= param(0, 1); if(lx < 0) lx = 0; break;
            case 'J':
                if(nparam == 0 || params[0] == 0){
                    erase(lx, ly, ncols * nrows);
                } else if(params[0] == 2){
                    clear_keep_pos();
                }
                break;
            case 'K':
                if(nparam == 0 || params[0] == 0){
                    erase(lx, ly, ncols - lx);
                } else if(params[0] == 2){
                    erase(0, ly, ncols);
                }
                break;
            default:
                break;
        }
    }

    // n celule goale incepand de la (c, r), in ordinea liniara
    void erase(int c, int r, int n){
        int index = r * ncols + c;
        for(int i = 0; i < n && index < ncols * nrows; i++, index++){
            set_cell(index % ncols, index / ncols, ' ');
        }
    }

    void clear_keep_pos(){
        int sx = lx, sy = ly;
        clear();
        lx = sx;
        ly = sy;
    }

    void sgr(){
        if(nparam == 0){
            reset_attr();
            return;
        }
        for(int i = 0; i < nparam; i++){
            int p = params[i];
            if(p == 0){
                reset_attr();
            } else if(p == 1){
                bold = true;
            } else if(p == 22){
                bold = false;
            } else if(p == 7){
                uint8_t t = fg; fg = bg; bg = t;
            } else if(p >= 30 && p <= 37){
                fg = ANSI_TO_VGA[p - 30];
            } else if(p == 39){
                fg = FG_DEFAULT;
            } else if(p >= 40 && p <= 47){
                bg = ANSI_TO_VGA[p - 40];
            } else if(p == 49){
                bg = BG_DEFAULT;
            } else if(p >= 90 && p <= 97){
                fg = ANSI_TO_VGA[p - 90] + 8;
            } else if(p >= 100 && p <= 107){
                bg = ANSI_TO_VGA[p - 100] + 8;
            }
        }
        apply_attr();
    }

    int ncols = 0;
    int nrows = 0;
    int lx = 0, ly = 0;                 // pozitia de scriere (putc)
    int cur_x = -1, cur_y = -1;         // cursorul desenat de set_cursor
    uint8_t fg = FG_DEFAULT, bg = BG_DEFAULT, attr = DEFAULT_ATTR;
    bool bold = false;
    int esc = 0;                        // 0 normal, 1 dupa ESC, 2 in secventa CSI
    int params[MAX_PARAMS] = {};
    int nparam = 0;
    uint32_t pal[16] = {};
    uint16_t cells[MAX_ROWS][MAX_COLS] = {};
};

FbConsole console;
bool active = false;

}

__init bool fbcon_init(){
    if(cmdline_has("nofb") || !fb_init()){
        return false;
    }
    console.setup();
    // VGA text nu se mai vede in mod grafic; ramane in registru, dar tace
    vga_disable();
    hal_register(HalKind::Console, &console, "fbcon");
    hal_register(HalKind::Framebuffer, fb_device(), "fb0");
    active = true;
    return true;
}

bool fbcon_active(){ return active; }
uint16_t fbcon_cell(int col, int row){ return console.cell(col, row); }
void fbcon_log_pos(int& col, int& row){ console.log_pos(col, row); }
uint32_t fbcon_palette(int idx){ return console.color(idx); }

#include "drivers/vga.h"
#include "arch/x86_64/paging.h"
#include "hal/console.h"
#include "io.h"

namespace {

uint16_t* const VGA_BUFFER = (uint16_t*)(HHDM_BASE + 0xB8000);

class VgaConsole final : public Console {
public:
    constexpr VgaConsole() = default;

    void clear() override {
        if(disabled){
            return;
        }
        for(int y = 0 ; y < INALTIME_ECRAN ; y++){
            for(int x = 0 ; x < LATIME_ECRAN; x++){
                int index = y * LATIME_ECRAN + x ;
                VGA_BUFFER[index] = (color << 8) | ' ';
            }
        }
        log_x = 0;
        log_y = 0;
    }

    void put_at(const char* text, int x, int y) override {
        if(disabled){
            return;
        }
        int index = y * LATIME_ECRAN + x ;

        for(int i = 0 ; text[i] != '\0'; i++){
            VGA_BUFFER[index] = (color << 8) | (uint8_t)text[i] ;
            index++;

            if(index >= LATIME_ECRAN * INALTIME_ECRAN){
                break;
            }
        }
    }

    void set_cursor(int x, int y) override {
        if(disabled){
            return;
        }
        uint16_t poz = y * LATIME_ECRAN + x;

        outb(0x3D4 , 0x0F);
        outb(0x3D5 , (uint8_t)(poz & 0xFF));
        outb(0x3D4 , 0x0E);
        outb(0x3D5 , (uint8_t)((poz >> 8) & 0xFF));
    }

    void set_color(uint8_t c) override { color = c; }

    void sync_cursor() override {
        int x = log_x < LATIME_ECRAN ? log_x : LATIME_ECRAN - 1;
        int y = log_y < INALTIME_ECRAN ? log_y : INALTIME_ECRAN - 1;
        set_cursor(x, y);
    }

    // \n, \r, \b, \t si secventele ANSI de care are nevoie un shell cu editare de linie: ESC [ n A/B/C/D,
    // ESC [ r ; c H, ESC [ J, ESC [ K (SGR si restul se ignora)
    void putc(char ch) override {
        if(disabled){
            return;
        }
        uint8_t c = (uint8_t)ch;
        if(esc == 1){
            if(c == '['){
                esc = 2;
                nparam = 0;
                params[0] = params[1] = 0;
            } else {
                esc = 0;
            }
            return;
        }
        if(esc == 2){
            if(c >= '0' && c <= '9'){
                if(nparam == 0){
                    nparam = 1;
                }
                params[nparam - 1] = params[nparam - 1] * 10 + (c - '0');
            } else if(c == ';'){
                if(nparam == 0){
                    nparam = 1;
                }
                if(nparam < 2){
                    nparam++;
                }
            } else if(c >= 0x40 && c <= 0x7E){
                esc = 0;
                csi(c);
            }
            return;
        }
        switch(c){
        case 0x1B:
            esc = 1;
            return;
        case '\n':
            log_x = 0;
            log_y++;
            break;
        case '\r':
            log_x = 0;
            return;
        case '\b':
            if(log_x > 0){
                log_x--;
            }
            return;
        case '\t':
            log_x = (log_x + 8) & ~7;
            if(log_x >= LATIME_ECRAN){
                log_x = 0;
                log_y++;
            }
            break;
        default:
            if(log_x >= LATIME_ECRAN){
                log_x = 0;
                log_y++;
                if(log_y >= INALTIME_ECRAN){
                    scroll();
                }
            }
            VGA_BUFFER[log_y * LATIME_ECRAN + log_x] = (color << 8) | c;
            log_x++;
            break;
        }
        if(log_y >= INALTIME_ECRAN){
            scroll();
        }
    }

    uint32_t flags() const override { return disabled ? 0 : CONSOLE_SCREEN; }
    void disable(){ disabled = true; }

    int cols() const override { return LATIME_ECRAN; }
    int rows() const override { return INALTIME_ECRAN; }

private:
    int param(int i, int dflt) const {
        return (i < nparam && params[i]) ? params[i] : dflt;
    }

    void erase(int from, int n){
        for(int i = from; i < from + n && i < LATIME_ECRAN * INALTIME_ECRAN; i++){
            VGA_BUFFER[i] = (color << 8) | ' ';
        }
    }

    void csi(uint8_t c){
        switch(c){
        case 'H':
        case 'f': {
            int r = param(0, 1) - 1;
            int cl = param(1, 1) - 1;
            log_y = r < 0 ? 0 : (r >= INALTIME_ECRAN ? INALTIME_ECRAN - 1 : r);
            log_x = cl < 0 ? 0 : (cl >= LATIME_ECRAN ? LATIME_ECRAN - 1 : cl);
            break;
        }
        case 'A': log_y -= param(0, 1); if(log_y < 0) log_y = 0; break;
        case 'B': log_y += param(0, 1); if(log_y >= INALTIME_ECRAN) log_y = INALTIME_ECRAN - 1; break;
        case 'C': log_x += param(0, 1); if(log_x >= LATIME_ECRAN) log_x = LATIME_ECRAN - 1; break;
        case 'D': log_x -= param(0, 1); if(log_x < 0) log_x = 0; break;
        case 'J': {
            int p = nparam ? params[0] : 0;
            if(p == 2){
                erase(0, LATIME_ECRAN * INALTIME_ECRAN);
            } else if(p == 0){
                int x = log_x < LATIME_ECRAN ? log_x : LATIME_ECRAN - 1;
                erase(log_y * LATIME_ECRAN + x, LATIME_ECRAN * INALTIME_ECRAN);
            }
            break;
        }
        case 'K': {
            int p = nparam ? params[0] : 0;
            int x = log_x < LATIME_ECRAN ? log_x : LATIME_ECRAN - 1;
            if(p == 2){
                erase(log_y * LATIME_ECRAN, LATIME_ECRAN);
            } else if(p == 0){
                erase(log_y * LATIME_ECRAN + x, LATIME_ECRAN - x);
            }
            break;
        }
        default:
            break;
        }
    }

    void scroll(){
        for(int i = 0; i < (INALTIME_ECRAN - 1) * LATIME_ECRAN; i++){
            VGA_BUFFER[i] = VGA_BUFFER[i + LATIME_ECRAN];
        }
        for(int x = 0; x < LATIME_ECRAN; x++){
            VGA_BUFFER[(INALTIME_ECRAN - 1) * LATIME_ECRAN + x] = (color << 8) | ' ';
        }
        log_y = INALTIME_ECRAN - 1;
    }

    bool disabled = false;
    int esc = 0;                        // 0 normal, 1 dupa ESC, 2 in secventa CSI
    int params[2] = {};
    int nparam = 0;
    uint8_t color = 0x0A;
    int log_x = 0;
    int log_y = 0;
};

VgaConsole vga_console;

}

void vga_init(){
    hal_register(HalKind::Console, &vga_console, "vga");
    vga_console.clear();
}

void vga_disable(){
    vga_console.disable();
}

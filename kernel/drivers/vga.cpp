#include "drivers/vga.h"
#include "hal/console.h"
#include "io.h"

namespace {

uint16_t* const VGA_BUFFER = (uint16_t*)(KERNEL_VMA + 0xB8000);

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

    void putc(char c) override {
        if(disabled){
            return;
        }
        if(c == '\n'){
            log_x = 0;
            log_y++;
        } else {
            VGA_BUFFER[log_y * LATIME_ECRAN + log_x] = (color << 8) | (uint8_t)c;
            if(++log_x >= LATIME_ECRAN){
                log_x = 0;
                log_y++;
            }
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

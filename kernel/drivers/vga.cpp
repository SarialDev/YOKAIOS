#include "drivers/vga.h"
#include "io.h"

static uint16_t* const VGA_BUFFER = (uint16_t*)(KERNEL_VMA + 0xB8000);
static const uint8_t culoare_alb_pe_negru = 0x0A;

static int log_x = 0;
static int log_y = 0;

void curata_ecran(){
    for(int y = 0 ; y < INALTIME_ECRAN ; y++){
        for(int x = 0 ; x < LATIME_ECRAN; x++){
            int index = y * LATIME_ECRAN + x ;
            VGA_BUFFER[index] = (culoare_alb_pe_negru << 8) | ' ';
        }
    }
    log_x = 0;
    log_y = 0;
}

void out(const char* text, int x, int y){
    int index = y * LATIME_ECRAN + x ;

    for(int i = 0 ; text[i] != '\0'; i++){
        VGA_BUFFER[index] = (culoare_alb_pe_negru << 8) | text[i] ;
        index++;

        if(index >= LATIME_ECRAN * INALTIME_ECRAN){
            break;
        }
    }
}

void muta_cursor(int x , int y){
    uint16_t poz = y * LATIME_ECRAN + x;

    outb(0x3D4 , 0x0F);
    outb(0x3D5 , (uint8_t)(poz & 0xFF));
    outb(0x3D4 , 0x0E);
    outb(0x3D5 , (uint8_t)((poz >> 8) & 0xFF));
}

static void scroll(){
    for(int i = 0; i < (INALTIME_ECRAN - 1) * LATIME_ECRAN; i++){
        VGA_BUFFER[i] = VGA_BUFFER[i + LATIME_ECRAN];
    }
    for(int x = 0; x < LATIME_ECRAN; x++){
        VGA_BUFFER[(INALTIME_ECRAN - 1) * LATIME_ECRAN + x] = (culoare_alb_pe_negru << 8) | ' ';
    }
    log_y = INALTIME_ECRAN - 1;
}

void vga_putc(char c){
    if(c == '\n'){
        log_x = 0;
        log_y++;
    } else {
        VGA_BUFFER[log_y * LATIME_ECRAN + log_x] = (culoare_alb_pe_negru << 8) | (uint8_t)c;
        if(++log_x >= LATIME_ECRAN){
            log_x = 0;
            log_y++;
        }
    }
    if(log_y >= INALTIME_ECRAN){
        scroll();
    }
}

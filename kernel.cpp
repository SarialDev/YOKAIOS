typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

extern "C" {
    uint8_t inb(uint16_t port);
    void outb(uint16_t port , uint8_t date);

}

uint16_t* const VGA_BUFFER = (uint16_t*)0xB8000;
const int LATIME_ECRAN = 80;
const int INALTIME_ECRAN = 25;
const uint8_t culoare_alb_pe_negru = 0x0A;

void curata_ecran(){
    for(int y = 0 ; y < INALTIME_ECRAN ; y++){
        for(int x = 0 ; x < LATIME_ECRAN; x++){
            int index = y * LATIME_ECRAN + x ;
            VGA_BUFFER[index] = (culoare_alb_pe_negru << 8) | ' ';
        }
    }
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

uint8_t inscan(){
    while((inb(0x64) & 1) == 0){

    }
    return inb(0x60);
}

char scanASCII(uint8_t scancode){
    switch(scancode){
        case 0x1E: return 'a';
        case 0x30: return 'b';
        case 0x2E: return 'c';
        case 0x20: return 'd';
        case 0x12: return 'e';
        case 0x21: return 'f';
        case 0x39: return ' ';
        default: return 0;
    }
}


extern "C" void kernel_main(){
    curata_ecran();

    out("YOKAI OS has booted succesfully !" , 2 , 2);
    out("yokai@drone_terminal:~$ " , 2 , 4);


    int cursor_x = 26;
    int cursor_y = 4; 

    while(1){
        uint8_t scancode = inscan();

        if(scancode < 0x80){
            char c = scanASCII(scancode);
            if(c != 0){
                char sir[2] = {c , '\0'};
                out(sir , cursor_x , cursor_y);
                cursor_x++;
                if(cursor_x >= LATIME_ECRAN) {
                    cursor_x = 0;
                    cursor_y++;
                }
            }
        }
    }
   /*for(;;) {
        __asm__ volatile("hlt");
    }*/ 

}
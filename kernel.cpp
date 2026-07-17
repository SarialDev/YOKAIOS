typedef unsigned short uint16_t;
typedef unsigned char uint8_t;

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

extern "C" void kernel_main(){
    curata_ecran();

    out("YOKAI OS has booted succesfully !" , 2 , 2);
    out("yokai@drone_terminal:~$ _" , 2 , 4);

    for(;;) {
        __asm__ volatile("hlt");
    }
}
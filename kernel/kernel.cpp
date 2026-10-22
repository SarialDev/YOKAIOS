typedef unsigned short uint16_t;
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;

//DECLARARE VARIABILE GLOBALE
int cursor_x = 26;
int cursor_y = 4;
char comanda_buffer[256];
int comanda_lungime = 0;
int TERMINALBOOL = 0 ; // depinde de valoare poate reprezenta daca e pe aplicatie sau in terminal



//GLOBAL VAR

//VARIABILE CULORI

#pragma pack(push , 1)
struct IDTentry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t always0;
    uint8_t flags;
    uint16_t base_high;
};
#pragma pack(pop)
#pragma pack(push, 1)
struct IDTptr {
    uint16_t limit ;
    uint32_t base;
};
#pragma pack(pop)

struct IDTentry idt[256];
struct IDTptr idtp;




extern "C" {
    uint8_t inb(uint16_t port);
    void outb(uint16_t port , uint8_t date);
    void incarcare_idt(uint32_t ptr_adresa);
    void tastatura_handler_asm();
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
//
//#ifdef _WIN32
//	system("cls");
//#else
//  system("clear");
//#endif
//



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

uint8_t inscan(){
    while((inb(0x64) & 1) == 0){

    }
    return inb(0x60);
}

void seteaza_poarta_idt(uint8_t numar , uint32_t baza , uint16_t sel , uint8_t flags){
    idt[numar].base_low = baza & 0xFFFF;
    idt[numar].base_high = (baza >> 16 ) & 0xFFFF;
    idt[numar].sel = sel;
    idt[numar].always0 = 0;
    idt[numar].flags = flags;
}

void initializare_idt(){
    idtp.limit = (sizeof(struct IDTentry) * 256) - 1;
    idtp.base = (uint32_t)&idt;

    for(int i = 0 ; i < 256 ;i++) {
        seteaza_poarta_idt(i,0,0,0);
        
    }
    incarcare_idt((uint32_t)&idtp);
    seteaza_poarta_idt(33 , (uint32_t)tastatura_handler_asm , 0x08 , 0x8E) ;
    incarcare_idt((uint32_t)&idtp);
    
}

char scanASCII(uint8_t scancode){
    switch(scancode){
        case 0x1E: return 'a';
        case 0x30: return 'b';
        case 0x2E: return 'c';
        case 0x20: return 'd';
        case 0x12: return 'e';
        case 0x21: return 'f';
        case 0x10: return 'q';
        case 0x11: return 'w';
        case 0x13: return 'r';
        case 0x14: return 't';
        case 0x15: return 'y';
        case 0x16: return 'u';
        case 0x17: return 'i';
        case 0x18: return 'o';
        case 0x19: return 'p';
        case 0x1A: return '[';
        case 0x1B: return ']';
        case 0x1C: return '\n';
        case 0x1F: return 's';
        case 0x22: return 'g';
        case 0x23: return 'h';
        case 0x24: return 'j';
        case 0x25: return 'k';
        case 0x26: return 'l';
        case 0x27: return ';';
        case 0x28: return '"';
        case 0x2C: return 'z';
        case 0x2D: return 'x';
        case 0x2F: return 'v';
        case 0x31: return 'n';
        case 0x32: return 'm';
        case 0x33: return ',';
        case 0x34: return '.';
        case 0x35: return '/';
        case 0x02: return '1';
        case 0x03: return '2';
        case 0x04: return '3';
        case 0x05: return '4';
        case 0x06: return '5';
        case 0x07: return '6';
        case 0x08: return '7';
        case 0x09: return '8';
        case 0x0A: return '9';
        case 0x0B: return '0';
        case 0x0C: return '-';
        case 0x0D: return '=';
        case 0x0F: return '\t';
        case 0x39: return ' ';

        default: return 0;
    }
}

// VARIABILE TIN
char raw_buffer[256];
volatile int raw_len = 0 ; 
volatile bool cin_gata = false; 

void citeste_linia(){
    raw_len = 0 ;
    cin_gata = false ; 
    while(!cin_gata) {
        __asm__ volatile("hlt");
    }
}


class KernelCin {
    public:
        KernelCin& operator>>(char* dest) {
            citeste_linia();
            int i = 0 ; 
            while(i < raw_len) {
                dest[i] = raw_buffer[i] ; 
                i++;
            }
            dest[i] = '\0';
            return *this;
        }

        KernelCin& operator>>(char& c) {
        citeste_linia();
        if(raw_len > 0) c = raw_buffer[0];
        else c = '\0';
        return *this;
    }

        KernelCin& operator>>(int& num) {
            citeste_linia();
            num = 0 ; 
            int i = 0 ; 
            bool negativ = false;
            if(raw_buffer[0] == '-') {
                negativ = true ; 
                i = 1;
            }
            while(i < raw_len && raw_buffer[i] >= '0' && raw_buffer[i] <= '9'){
                num = num * 10 + (raw_buffer[i] - '0');
                i++;
            }
            if(negativ) num = -num;
            return *this;
        }


};

KernelCin tin ; 

bool sunt_egale(const char* sir1 , const char* sir2){
    int i = 0;
    while(sir1[i] != '\0' && sir2[i] != '\0'){
        if(sir1[i] != sir2[i]){
            return false;
        }
        i++;
    }
    return sir1[i] == sir2[i];
}

void executa_comanda(const char* comanda , int& cursor_x , int& cursor_y , int comanda_lungime){
    cursor_y++;
    cursor_x = 0;
    
    if(comanda_lungime == 0){

    }else if (sunt_egale(comanda , "help") && TERMINALBOOL == 0){
        out("Available commands: help , clear , version" , cursor_x, cursor_y);
        cursor_y++;
    }
    else if(sunt_egale(comanda , "clear") ){
        curata_ecran();
        cursor_y = 1;
    } else if (sunt_egale(comanda , "version")  ){
        out("YOKAI OS version 0.1" , cursor_x , cursor_y);
        cursor_y++;
    } else if(sunt_egale(comanda , "color ")){

    } else if (TERMINALBOOL == 0) {
        out("command not found:" , cursor_x , cursor_y);
        cursor_y++;
    }
    
    
    
    
}

void remapare_pic(){
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    
    outb(0x21, 0xFD);
    outb(0xA1, 0xFF);
}




extern "C" void tastatura_handler_cpp(){
    uint8_t scancode = inb(0x60);

    if(scancode < 0x80){
        if(scancode == 0x1C){
            /*for(int i = 0 ; i <= comanda_lungime ; i++){
            comanda_buffer[i] = '\0';            
            }
            executa_comanda(" ", cursor_x , cursor_y , comanda_lungime);*/
            comanda_buffer[comanda_lungime] = '\0';
    executa_comanda(comanda_buffer , cursor_x , cursor_y , comanda_lungime);
    comanda_lungime = 0;
    out("yokai@drone_terminal:~$ " , 2 , cursor_y);    
    
    //out("yokai@drone_terminal:~$ " , 2 , cursor_y);
    cursor_x = 26;
    for(int i = 0 ; i < 256 ; i++){
        comanda_buffer[i] = '\0';
    }
    muta_cursor(cursor_x , cursor_y);
        }else if (scancode == 0x0E){
            if(comanda_lungime > 0) {
                comanda_lungime--;
                cursor_x--;
                out(" ", cursor_x , cursor_y);
                muta_cursor(cursor_x , cursor_y);
                
            }
        } else {
            char c = scanASCII(scancode);
            if(c != 0 && comanda_lungime < 255) {
                comanda_buffer[comanda_lungime] = c ;
                comanda_lungime++;
                
                char sir[2] = {c, '\0'};
                out(sir , cursor_x , cursor_y);
                cursor_x++;
                muta_cursor(cursor_x , cursor_y);
            }
        }
    }
}

extern "C" void kernel_main(){
    curata_ecran();

    out("YOKAI OS has booted succesfully !" , 2 , 2);
    out("yokai@drone_terminal:~$ " , 2 , 4);
    muta_cursor(cursor_x , cursor_y);


    remapare_pic();
    initializare_idt();

    __asm__ volatile("sti");

    while(1){
        __asm__ volatile("hlt");
    }

     
/*
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
            } if(scancode == 0x1C){
                comanda_buffer[comanda_lungime] = '\0';
                executa_comanda(comanda_buffer , cursor_x , cursor_y , comanda_lungime);
                comanda_lungime = 0;
                out("yokai@drone_terminal:~$ " , 2 , cursor_y);
                cursor_x = 26;
                
                for(int i = 0 ; i < 256 ; i++){
                    comanda_buffer[i] = '\0';
                }
            } else if(scancode == 0x0E) { 
                if(comanda_lungime > 0){
                    comanda_lungime--;
                    cursor_x--;
                    if(cursor_x < 0){
                        cursor_x = LATIME_ECRAN - 1;
                        cursor_y--;
                    }
                    out(" " , cursor_x , cursor_y);
                }
                
            } else {
                char c = scanASCII(scancode);
                if(c != 0 && comanda_lungime < 255) {
                    comanda_buffer[comanda_lungime] = c; 
                    comanda_lungime++;

                    char sir[2] = {c , '\0'};
                    out(sir , cursor_x , cursor_y);
                    cursor_x++;
                }
            }
        }
    }
   for(;;) {
        __asm__ volatile("hlt");
    }*/ 

}
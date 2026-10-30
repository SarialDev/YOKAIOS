#include "types.h"
#include "io.h"
#include "drivers/vga.h"
#include "drivers/fbcon.h"
#include "drivers/fb/fb.h"
#include "drivers/serial.h"
#include "hal/hal.h"
#include "hal/console.h"
#include "hal/irqchip.h"
#include "hal/input.h"
#include "arch/x86_64/irqchip.h"
#include "time/tsc.h"
#include "tests/test_hal.h"
#include "lib/kprintf.h"
#include "boot/multiboot2.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/percpu.h"
#include "arch/x86_64/smp.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/pic.h"
#include "drivers/ps2kbd.h"
#include "drivers/ps2mouse.h"
#include "drivers/pci/pci.h"
#include "tests/test_pci.h"
#include "tests/test_ps2mouse.h"
#include "tests/test_fb.h"
#include "tests/test_rtc.h"
#include "drivers/rtc.h"
#include "drivers/usb/usb.h"
#include "tests/test_usb.h"
#include "acpi/acpi.h"
#include "arch/x86_64/lapic.h"
#include "arch/x86_64/ioapic.h"
#include "time/clock.h"
#include "tests/test_cpu.h"
#include "tests/test_mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "mm/kmalloc.h"
#include "mm/fault.h"
#include "core/panic.h"
#include "sched/task.h"
#include "sched/sched.h"
#include "tests/test_sched.h"
#include "tests/test_sync.h"
#include "tests/test_user.h"
#include "tests/test_rt.h"
#include "tests/test_proc.h"
#include "tests/test_smp.h"
#include "syscall/syscall.h"

// Ecranul de text vine din HAL (VGA)
Console* screen = nullptr;

void out(const char* text, int x, int y){
    if(screen){
        screen->put_at(text, x, y);
    }
}

void curata_ecran(){
    if(screen){
        screen->clear();
    }
}

void muta_cursor(int x, int y){
    if(screen){
        screen->set_cursor(x, y);
    }
}

//DECLARARE VARIABILE GLOBALE
int cursor_x = 26;
int cursor_y = 4;
char comanda_buffer[256];
int comanda_lungime = 0;
int TERMINALBOOL = 0 ; // depinde de valoare poate reprezenta daca e pe aplicatie sau in terminal



//GLOBAL VAR

//VARIABILE CULORI

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

// Logo „YOKAI” din blocuri (0xDB = █ in CP437/VGA, sintetizat si de fbcon), rosu deschis; 5 randuri
void afiseaza_logo(int x, int y){
    static const char* const litere[5][5] = {
        {"#   #", " # # ", "  #  ", "  #  ", "  #  "},      // Y
        {" ### ", "#   #", "#   #", "#   #", " ### "},      // O
        {"#   #", "#  # ", "###  ", "#  # ", "#   #"},      // K
        {" ### ", "#   #", "#####", "#   #", "#   #"},      // A
        {"#####", "  #  ", "  #  ", "  #  ", "#####"},      // I
    };
    if(!screen){
        return;
    }
    hal_console_set_color(0x0C);
    for(int r = 0; r < 5; r++){
        char linie[5 * 6 + 1];
        int n = 0;
        for(int l = 0; l < 5; l++){
            for(int c = 0; c < 5; c++){
                linie[n++] = litere[l][r][c] == '#' ? (char)0xDB : ' ';
            }
            linie[n++] = ' ';
        }
        linie[n] = '\0';
        out(linie, x, y + r);
    }
    hal_console_set_color(0x0F);
}

// "up 12.345 s (jiffies 12345)"
void afiseaza_uptime(int x, int y){
    char buf[64];
    int n = 0;
    auto put_num = [&](uint64_t v, int min_digits){
        char tmp[20];
        int len = 0;
        do {
            tmp[len++] = '0' + v % 10;
            v /= 10;
        } while(v || len < min_digits);
        while(len){
            buf[n++] = tmp[--len];
        }
    };
    auto put_str = [&](const char* s){
        while(*s){
            buf[n++] = *s++;
        }
    };
    uint64_t ms = uptime_ms();
    put_str("up ");
    put_num(ms / 1000, 1);
    put_str(".");
    put_num(ms % 1000, 3);
    put_str(" s (jiffies ");
    put_num(jiffies, 1);
    put_str(")");
    buf[n] = '\0';
    out(buf, x, y);
}

// "date": ora reala UTC din RTC + uptime
void afiseaza_date(int x, int y){
    char buf[32];
    rtc_format(rtc_from_epoch(wall_time()), buf, sizeof(buf));
    out(buf, x, y);
}

// lspci / lspci -v: o linie pe rand; cand ecranul se umple, il curata
void afiseaza_lspci(bool verbose, int& cursor_y){
    char buf[LATIME_ECRAN];
    auto linie = [&](const char* text){
        if(cursor_y >= INALTIME_ECRAN - 1){
            curata_ecran();
            cursor_y = 1;
        }
        out(text, 0, cursor_y++);
    };
    for(int i = 0; i < pci_device_count(); i++){
        const PciDevice* d = pci_device_at(i);
        pci_format(d, buf, sizeof(buf));
        linie(buf);
        for(int b = 0; verbose && b < PCI_BAR_COUNT; b++){
            if(pci_format_bar(d, b, buf, sizeof(buf))){
                linie(buf);
            }
        }
    }
}

void executa_comanda(const char* comanda , int& cursor_x , int& cursor_y , int comanda_lungime){
    cursor_y++;
    cursor_x = 0;
    
    if(comanda_lungime == 0){

    }else if (sunt_egale(comanda , "help") && TERMINALBOOL == 0){
        out("Available commands: help , clear , version , uptime , date , lspci" , cursor_x, cursor_y);
        cursor_y++;
    }
    else if(sunt_egale(comanda , "clear") ){
        curata_ecran();
        cursor_y = 1;
    } else if (sunt_egale(comanda , "version")  ){
        out("YOKAI OS version 0.1" , cursor_x , cursor_y);
        cursor_y++;
    } else if (sunt_egale(comanda , "uptime")){
        afiseaza_uptime(cursor_x , cursor_y);
        cursor_y++;
    } else if (sunt_egale(comanda , "date")){
        afiseaza_date(cursor_x , cursor_y);
        cursor_y++;
    } else if (sunt_egale(comanda , "lspci") || sunt_egale(comanda , "lspci -v")){
        afiseaza_lspci(comanda[5] != '\0', cursor_y);
    } else if(sunt_egale(comanda , "color ")){

    } else if (TERMINALBOOL == 0) {
        out("command not found:" , cursor_x , cursor_y);
        cursor_y++;
    }
    
    
    
    
}

// Apelat din IRQ1 (ps2kbd) pentru fiecare tasta apasata; k.ascii are deja Shift/Caps aplicate
void shell_on_key(const KeyEvent& k){
    if(k.keycode == KEY_ENTER){
        comanda_buffer[comanda_lungime] = '\0';
        executa_comanda(comanda_buffer , cursor_x , cursor_y , comanda_lungime);
        comanda_lungime = 0;
        if(screen && cursor_y >= screen->rows()){
            curata_ecran();
            cursor_y = 0;
        }
        out("yokai@drone_terminal:~$ " , 2 , cursor_y);
        cursor_x = 26;
        for(int i = 0 ; i < 256 ; i++){
            comanda_buffer[i] = '\0';
        }
        muta_cursor(cursor_x , cursor_y);
    } else if(k.keycode == KEY_BACKSPACE){
        if(comanda_lungime > 0) {
            comanda_lungime--;
            cursor_x--;
            out(" ", cursor_x , cursor_y);
            muta_cursor(cursor_x , cursor_y);
        }
    } else if(k.ascii != 0 && comanda_lungime < 255) {
        comanda_buffer[comanda_lungime] = k.ascii;
        comanda_lungime++;

        char sir[2] = {k.ascii, '\0'};
        out(sir , cursor_x , cursor_y);
        cursor_x++;
        muta_cursor(cursor_x , cursor_y);
    }
}

// Evenimentele de tastatura de pe event bus; shell-ul primeste doar apasarile
void shell_on_input(const InputEvent& ev){
    if(ev.type == InputType::Key && ev.key.pressed){
        shell_on_key(ev.key);
    }
}

volatile bool breakpoint_hit = false;

// name apare in lista din "test=" (separata prin virgule, pana la spatiu), ex. "test=cpu,mm"
bool test_named(const char* value, const char* name){
    while(value && *value && *value != ' '){
        const char* n = name;
        while(*n && *value == *n){
            value++;
            n++;
        }
        if(!*n && (*value == ',' || *value == ' ' || !*value)){
            return true;
        }
        while(*value && *value != ',' && *value != ' '){
            value++;
        }
        if(*value == ','){
            value++;
        }
    }
    return false;
}

void breakpoint_handler(InterruptFrame* f){
    breakpoint_hit = true;
    kprintf("int3 la rip=%lx\n", f->rip);
}

extern "C" void kernel_main(uint32_t magic, uint64_t mbi_phys){
    // Inaintea oricarui lock: cpu_id() si task_current() citesc PerCpu prin GS
    percpu_init(0, 0);
    vga_init();
    screen = hal_console_screen();

    if(serial_init()){
        kprintf("[OK] serial\n");
    }
    kprintf("YOKAI OS booting (x86-64, kernel %p)\n", (void*)&kernel_main);

    if(multiboot2_parse(magic, mbi_phys)){
        multiboot2_print();
        kprintf("[OK] multiboot2\n");
    }

    gdt_init_cpu(cpus[0]);
    kprintf("[OK] gdt\n");

    idt_init();
    irq_register(3, breakpoint_handler);
    __asm__ volatile("int $3");
    if(breakpoint_hit){
        kprintf("[OK] idt\n");
    }
    fault_init();
    kprintf("[OK] stackguard (%s)\n", stack_guard_source());

    pmm_init();
    vmm_init();
    kmalloc_init();
    // Consola grafica peste framebuffer; fara el (sau cu "nofb") ramane VGA text. Mesajele de pana aici au mers doar pe serial
    if(fbcon_init()){
        screen = hal_console_screen();
        kprintf("[OK] fb %ux%ux32 %s%s\n", fb_width(), fb_height(), fb_write_combining() ? "wc" : "uc",
                fb_has_backbuffer() ? " backbuffer" : "");
    } else {
        kprintf("[OK] vgacon (fara framebuffer)\n");
    }
    tasks_init();
    sched_init();
    syscall_init();

    // Test manual pentru panic: cmdline "crash" forteaza #DE
    if(cmdline_has("crash")){
        // asm direct: gcc transforma 1/x in comparatii si nu mai emite div
        __asm__ volatile("xor %%ecx, %%ecx; mov $1, %%eax; xor %%edx, %%edx; div %%ecx" : : : "eax", "ecx", "edx");
    }

    pic_init();
    irqchip_pic_register();
    kprintf("[OK] pic\n");

    if(acpi_init()){
        kprintf("[OK] acpi\n");
        int cpus = 0;
        for(int i = 0; i < acpi_madt.cpu_count; i++){
            const AcpiCpu& c = acpi_madt.cpus[i];
            if(c.flags & 3){
                cpus++;
            }
            kprintf("  CPU acpi=%u lapic=%u flags=%x\n", c.acpi_id, c.lapic_id, c.flags);
        }
        for(int i = 0; i < acpi_madt.ioapic_count; i++){
            const AcpiIoApic& io = acpi_madt.ioapics[i];
            kprintf("  IOAPIC id=%u addr=%x gsi=%u\n", io.id, io.addr, io.gsi_base);
        }
        for(int i = 0; i < acpi_madt.override_count; i++){
            const AcpiOverride& o = acpi_madt.overrides[i];
            kprintf("  ISO irq=%u -> gsi=%u flags=%x\n", o.irq, o.gsi, o.flags);
        }
        lapic_init(acpi_madt.lapic_addr);
        ioapic_init();
        irqchip_apic_register();
        kprintf("CPU-uri: %d, BSP lapic=%u\n", cpus, hal_irqchip()->cpu_id());
        kprintf("[OK] apic\n");
    }
    tsc_register();
    clock_init();
    rtc_init();
    smp_init();
    pci_init();
    input_subscribe(shell_on_input);
    ps2kbd_init();
    kprintf("[OK] keyboard\n");
    ps2mouse_init();
    kprintf("[OK] mouse\n");
    usb_init();
    hal_list();
    kprintf("[OK] hal\n");
    kprintf("[OK] rtc\n");

    // De aici nu se mai apeleaza cod __init
    fault_protect_kernel();

    const char* test = cmdline_get("test=");
    if(test_named(test, "cpu")){
        test_cpu_run();
    }
    if(test_named(test, "mm")){
        test_mm_run();
    }
    if(test_named(test, "sched")){
        test_sched_run();
    }
    if(test_named(test, "sync")){
        test_sync_run();
    }
    if(test_named(test, "smp")){
        test_smp_run();
    }
    if(test_named(test, "user")){
        test_user_run();
    }
    if(test_named(test, "proc")){
        test_proc_run();
    }
    if(test_named(test, "rt")){
        test_rt_run();
    }
    if(test_named(test, "hal")){
        test_hal_run();
    }
    if(test_named(test, "pci")){
        test_pci_run();
    }
    if(test_named(test, "mouse")){
        test_ps2mouse_run();
    }
    if(test_named(test, "fb")){
        test_fb_run();
    }
    if(test_named(test, "rtc")){
        test_rtc_run();
    }
    if(test_named(test, "usb")){
        test_usb_run();
    }
    if(test_named(test, "relock")){
        test_spin_relock();
    }
    if(test_named(test, "smash")){
        test_stack_smash();
    }

    kprintf("[OK] boot\n");
    if(cmdline_has("test")){
        outb(0xF4, 0x10);   // isa-debug-exit: QEMU iese cu codul 33
    }

    curata_ecran();

    afiseaza_logo(2, 1);
    out("YOKAI OS has booted succesfully !" , 2 , 7);
    cursor_y = 9;
    out("yokai@drone_terminal:~$ " , 2 , cursor_y);
    muta_cursor(cursor_x , cursor_y);


    // Tastele vin acum pe IRQ1; task 0 devine idle si doarme intre intreruperi
    sched_idle();

     
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
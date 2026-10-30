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
#include "tests/test_drivers.h"
#include "tests/test_vfs.h"
#include "fs/vfs/vfs.h"
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
#include "fs/vfs/vfs.h"
#include "fs/tarfs.h"
#include "fs/pseudofs.h"
#include "sync/semaphore.h"
#include "tests/test_tarfs.h"
#include "tests/test_tmpfs.h"
#include "tests/test_devfs.h"

// Ecranul de text vine din HAL (VGA)
Console* screen = nullptr;

// Doar pentru teste: output-ul shell-ului se oglindeste pe consolele fara ecran (serial)
volatile bool shell_serial_echo = false;
volatile int shell_command_count = 0;
volatile int shell_done_count = 0;
// cat / ls ating VFS-ul (mutex): ruleaza intr-un task, iar shell-ul ignora tastele pana termina
static volatile bool shell_busy = false;
static Semaphore fs_cmd_sem;
static char fs_cmd[256];
static bool in_command = false;

void out(const char* text, int x, int y){
    if(screen){
        screen->put_at(text, x, y);
    }
    if(shell_serial_echo && in_command){
        for(int i = 0; Console* c = hal_console(i); i++){
            if(!(c->flags() & CONSOLE_SCREEN)){
                for(const char* t = text; *t; t++){
                    c->putc(*t);
                }
                c->putc('\n');
            }
        }
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

bool starts_with(const char* s , const char* prefix){
    while(*prefix && *s == *prefix){
        s++;
        prefix++;
    }
    return *prefix == '\0';
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

// true = comanda continua intr-un task (shell_fs_worker), care tipareste si prompt-ul
bool executa_comanda(const char* comanda , int& cursor_x , int& cursor_y , int comanda_lungime){
    bool deferred = false;
    cursor_y++;
    cursor_x = 0;
    shell_command_count++;
    in_command = true;
    
    if(comanda_lungime == 0){

    }else if (sunt_egale(comanda , "help") && TERMINALBOOL == 0){
        out("Available commands: help , clear , version , uptime , date , lspci , ls , cat , echo" , cursor_x, cursor_y);
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
    } else if(starts_with(comanda , "cat ") || sunt_egale(comanda , "ls") || starts_with(comanda , "ls ") ||
              sunt_egale(comanda , "echo") || starts_with(comanda , "echo ")){
        for(int i = 0 ; i < 255 && comanda[i] ; i++){
            fs_cmd[i] = comanda[i];
            fs_cmd[i + 1] = '\0';
        }
        shell_busy = true;
        deferred = true;
        fs_cmd_sem.up();
    } else if(sunt_egale(comanda , "color ")){

    } else if (TERMINALBOOL == 0) {
        out("command not found:" , cursor_x , cursor_y);
        cursor_y++;
    }
    in_command = false;
    return deferred;
}

// Dupa o comanda: linie noua cu prompt
void shell_finish(){
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
    shell_done_count++;
}

// Apelat din IRQ1 (ps2kbd) pentru fiecare tasta apasata; k.ascii are deja Shift/Caps aplicate
void shell_on_key(const KeyEvent& k){
    if(shell_busy){
        return;
    }
    if(k.keycode == KEY_ENTER){
        comanda_buffer[comanda_lungime] = '\0';
        if(!executa_comanda(comanda_buffer , cursor_x , cursor_y , comanda_lungime)){
            shell_finish();
        }
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

// "pre" + path + ": " + msg (taiat la latimea ecranului)
static void snprint_err(const char* pre , const char* path , const char* msg , char* out_line){
    int n = 0;
    auto add = [&](const char* t){
        while(*t && n < LATIME_ECRAN - 1){
            out_line[n++] = *t++;
        }
    };
    add(pre);
    add(path);
    add(": ");
    add(msg);
    out_line[n] = '\0';
}

// Linie de iesire pentru cat / ls; cand ecranul se umple, il curata (ca lspci)
static void fs_line(const char* text){
    if(cursor_y >= INALTIME_ECRAN - 1){
        curata_ecran();
        cursor_y = 1;
    }
    out(text , 0 , cursor_y++);
}

static const char* fs_error(int64_t e){
    switch(-e){
    case ENOENT: return "No such file or directory";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case ELOOP: return "Too many symbolic links";
    case ENAMETOOLONG: return "File name too long";
    case EROFS: return "Read-only file system";
    case ENOSPC: return "No space left on device";
    case EPERM: return "Operation not permitted";
    case EEXIST: return "File exists";
    case ENOMEM: return "Out of memory";
    case EFBIG: return "File too large";
    default: return "I/O error";
    }
}

static void fs_cat(const char* path){
    static char data[512];
    File* f;
    int err = vfs_open(Path{} , path , O_RDONLY , 0 , f);
    if(err){
        char line[LATIME_ECRAN + 1];
        snprint_err("cat: " , path , fs_error(err) , line);
        fs_line(line);
        return;
    }
    char line[LATIME_ECRAN];
    int n = 0;
    uint64_t total = 0;
    int64_t got = 0;
    while(total < 4096 && (got = f->ops->read(f , data , sizeof(data) , false)) > 0){
        for(int64_t i = 0 ; i < got ; i++){
            char c = data[i];
            if(c == '\n'){
                line[n] = '\0';
                fs_line(line);
                n = 0;
                continue;
            }
            line[n++] = (c >= ' ' && c < 127) ? c : '.';
            if(n == LATIME_ECRAN - 1){
                line[n] = '\0';
                fs_line(line);
                n = 0;
            }
        }
        total += (uint64_t)got;
    }
    if(got < 0){
        fs_line(fs_error(got));
    } else if(n){
        line[n] = '\0';
        fs_line(line);
    }
    file_put(f);
}

static void fs_ls(const char* path){
    static char data[1024];
    File* f;
    int err = vfs_open(Path{} , path , O_RDONLY | O_DIRECTORY , 0 , f);
    if(err){
        char line[LATIME_ECRAN + 1];
        snprint_err("ls: " , path , fs_error(err) , line);
        fs_line(line);
        return;
    }
    char line[LATIME_ECRAN];
    int n = 0;
    int64_t got;
    while((got = f->ops->getdents(f , data , sizeof(data))) > 0){
        for(int64_t off = 0 ; off < got ;){
            Dirent64* d = (Dirent64*)(data + off);
            off += d->d_reclen;
            const char* nm = d->d_name;
            int len = 0;
            while(nm[len]){
                len++;
            }
            if((nm[0] == '.' && (len == 1 || (len == 2 && nm[1] == '.'))) || len > LATIME_ECRAN - 4){
                continue;
            }
            if(n + len + 2 > LATIME_ECRAN - 1){
                line[n] = '\0';
                fs_line(line);
                n = 0;
            }
            for(int i = 0 ; i < len ; i++){
                line[n++] = nm[i];
            }
            if(d->d_type == DT_DIR){
                line[n++] = '/';
            }
            line[n++] = ' ';
        }
    }
    if(n){
        line[n] = '\0';
        fs_line(line);
    }
    file_put(f);
}

// echo [text] [> | >> path]: fara redirectare scrie textul pe ecran, altfel in fisier (creat daca lipseste)
static void fs_echo(const char* arg){
    char text[256];
    int n = 0;
    const char* redir = nullptr;
    bool append = false;
    for(const char* p = arg ; *p ; p++){
        if(*p == '>'){
            append = p[1] == '>';
            redir = p + (append ? 2 : 1);
            break;
        }
        text[n++] = *p;
    }
    while(n > 0 && text[n - 1] == ' '){
        n--;
    }
    text[n] = '\0';
    if(!redir){
        fs_line(text);
        return;
    }
    while(*redir == ' '){
        redir++;
    }
    if(!*redir){
        fs_line("echo: missing file name");
        return;
    }
    File* f;
    int err = vfs_open(Path{} , redir , O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC) , 0644 , f);
    if(err){
        char line[LATIME_ECRAN + 1];
        snprint_err("echo: " , redir , fs_error(err) , line);
        fs_line(line);
        return;
    }
    text[n++] = '\n';
    int64_t w = f->ops->write(f , text , (uint64_t)n , false);
    if(w < 0){
        fs_line(fs_error(w));
    }
    file_put(f);
}

static void shell_fs_worker(void*){
    for(;;){
        fs_cmd_sem.down();
        in_command = true;
        cursor_x = 0;
        const char* arg = fs_cmd + (fs_cmd[0] == 'c' ? 3 : fs_cmd[0] == 'e' ? 4 : 2);
        while(*arg == ' '){
            arg++;
        }
        if(fs_cmd[0] == 'c'){
            fs_cat(arg);
        } else if(fs_cmd[0] == 'e'){
            fs_echo(arg);
        } else {
            fs_ls(*arg ? arg : "/");
        }
        in_command = false;
        shell_finish();
        shell_busy = false;
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
    vfs_init();
    tarfs_init();
    tmpfs_init();
    devfs_init();
    if(boot_find_module("initrd")){
        int err = tarfs_mount_root();
        if(err){
            kprintf("initrd: montarea pe / a esuat (%d)\n", err);
        } else {
            kprintf("[OK] initrd montat pe / (tarfs, read-only)\n");
            fs_mount_pseudo();
        }
    }
    Task* fs_worker = task_create_kernel("shell-fs", shell_fs_worker, nullptr);
    if(fs_worker){
        sched_add(fs_worker);
    }

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
    if(test_named(test, "drivers")){
        test_drivers_run();
    }
    if(test_named(test, "tarfs")){
        test_tarfs_run();
    }
    if(test_named(test, "tmpfs")){
        test_tmpfs_run();
    }
    if(test_named(test, "devfs")){
        test_devfs_run();
    }
    if(test_named(test, "vfs")){
        test_vfs_run();
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
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
#include "arch/x86_64/fpu.h"
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
#include "fs/fat32/fat32.h"
#include "fs/ext4/ext4.h"
#include "sync/semaphore.h"
#include "tests/test_tarfs.h"
#include "tests/test_tmpfs.h"
#include "tests/test_devfs.h"
#include "tests/test_block.h"
#include "tests/test_ahci.h"
#include "tests/test_fat32.h"
#include "tests/test_ext4.h"
#include "tests/test_ext4w.h"
#include "tests/test_fs.h"
#include "tests/test_libc.h"
#include "tests/test_coreutils.h"
#include "tests/test_perm.h"
#include "tests/test_crypto.h"
#include "tests/test_lua.h"
#include "tests/test_vm.h"
#include "tests/test_ypkg.h"
#include "tests/test_userspace.h"
#include "drivers/ahci/ahci.h"
#include "drivers/nvme/nvme.h"
#include "fs/iso9660.h"
#include "tests/test_nvme.h"
#include "tests/test_iso9660.h"
#include "block/blockdev.h"
#include "block/bcache.h"
#include "tests/test_procfs.h"
#include "tests/test_ysh.h"
#include "drivers/tty.h"
#include "exec/elf.h"


// Ecranul de text vine din HAL (VGA)
Console* screen = nullptr;

// Logo-ul si mesajul de boot se deseneaza cu put_at; dupa aceea ecranul apartine terminalului (drivers/tty.cpp)
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
    fpu_init();
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
    tty_init();
    ps2kbd_init();
    kprintf("[OK] keyboard\n");
    ps2mouse_init();
    kprintf("[OK] mouse\n");
    usb_init();
    ahci_init();
    nvme_init();
    hal_list();
    kprintf("[OK] hal\n");
    kprintf("[OK] rtc\n");
    vfs_init();
    tarfs_init();
    tmpfs_init();
    devfs_init();
    procfs_init();
    fat32_init();
    ext4_init();
    iso9660_init();
    // Radacina: cmdline root=<dispozitiv> (ext4, ex. root=/dev/sda2); daca lipseste sau nu se monteaza, initrd-ul
    bool have_root = false;
    const char* root_arg = cmdline_get("root=");
    if(root_arg){
        char root_dev[32];
        uint32_t rn = 0;
        while(root_arg[rn] && root_arg[rn] != ' ' && rn < sizeof(root_dev) - 1){
            root_dev[rn] = root_arg[rn];
            rn++;
        }
        root_dev[rn] = 0;
        const char* rw_arg = cmdline_get("rw");
        bool root_rw = rw_arg && (*rw_arg == 0 || *rw_arg == ' ');
        int err = ext4_mount_root(root_dev, root_rw);
        if(err){
            kprintf("root=%s: montarea ext4 a esuat (%d)%s\n", root_dev, err,
                    boot_find_module("initrd") ? ", folosesc initrd" : "");
        }
        have_root = err == 0;
    }
    if(!have_root && boot_find_module("initrd")){
        int err = tarfs_mount_root();
        if(err){
            kprintf("initrd: montarea pe / a esuat (%d)\n", err);
        } else {
            kprintf("[OK] initrd montat pe / (tarfs, read-only)\n");
            have_root = true;
        }
    }
    if(have_root){
        // Boot normal: /dev, /tmp, /run si /proc le monteaza /sbin/init. Testele (cmdline test=) le vor gata de la inceput,
        // in afara de "nopseudo" (test=ysh verifica montarile facute de init)
        if(cmdline_has("test") && !cmdline_has("nopseudo")){
            fs_mount_pseudo();
        }
        fat32_automount();
        iso9660_automount();
    }
    bcache_start_flusher();

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
    if(test_named(test, "block")){
        test_block_run();
    }
    if(test_named(test, "ahci")){
        test_ahci_run();
    }
    if(test_named(test, "nvme")){
        test_nvme_run();
    }
    if(test_named(test, "iso9660")){
        test_iso9660_run();
    }
    if(test_named(test, "fat32")){
        test_fat32_run();
    }
    if(test_named(test, "ext4")){
        test_ext4_run(cmdline_get("ext4mode="));
    }
    if(test_named(test, "ext4w")){
        test_ext4w_run();
    }
    if(test_named(test, "fs")){
        test_fs_run();
    }
    if(test_named(test, "procfs")){
        test_procfs_run();
    }
    if(test_named(test, "ysh")){
        test_ysh_run();
    }
    if(test_named(test, "libc")){
        test_libc_run();
    }
    if(test_named(test, "coreutils")){
        test_coreutils_run();
    }
    if(test_named(test, "lua")){
        test_lua_run();
    }
    if(test_named(test, "vm")){
        test_vm_run();
    }
    if(test_named(test, "ypkg")){
        test_ypkg_run();
    }
    if(test_named(test, "perm")){
        test_perm_run();
    }
    if(test_named(test, "crypto")){
        test_crypto_run();
    }
    if(test_named(test, "vfs")){
        test_vfs_run();
    }
    if(test_named(test, "userspace")){
        test_userspace_run();
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
    out("YOKAI OS has booted succesfully !", 2, 7);
    // Terminalul scrie de aici in jos; lipsa /sbin/init nu e fatala (ecranul ramane cu logo-ul)
    kwrite("\x1b[10;1H", 7);
    tty_activate();
    // cmdline "login": init porneste /bin/login in loc de ysh
    static const char* const init_argv[] = {"init", nullptr};
    static const char* const init_argv_login[] = {"init", "-l", nullptr};
    static const char* const init_envp[] = {"PATH=/bin:/sbin", "HOME=/root", "TERM=yokai", nullptr};
    if(!exec_spawn("/sbin/init", cmdline_has("login") ? init_argv_login : init_argv, init_envp)){
        kprintf("init: /sbin/init nu a pornit\n");
    }

    // Task 0 devine idle si doarme intre intreruperi
    sched_idle();
}

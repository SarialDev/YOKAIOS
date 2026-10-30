#include "tests/test_drivers.h"
#include "drivers/pci/pci.h"
#include "drivers/fb/fb.h"
#include "hal/fb.h"
#include "time/clock.h"
#include "lib/kprintf.h"
#include "core/panic.h"

extern volatile bool shell_serial_echo;
extern volatile int shell_command_count;

namespace {

#define CHECK(cond) do { if(!(cond)) panic("test_drivers: %s (linia %d)", #cond, __LINE__); } while(0)

const uint16_t VENDOR_INTEL = 0x8086;
const uint16_t DEV_E1000 = 0x100e;
const uint32_t INPUT_TIMEOUT_MS = 30000;

void check_lspci(){
    kprintf("lspci:\n");
    pci_dump(false);
    CHECK(pci_find(VENDOR_INTEL, DEV_E1000));
    kprintf("[OK] test_drivers lspci e1000\n");
}

void check_fb(){
    Framebuffer* dev = hal_fb(0);
    if(!fb_ready() || !dev){            // "nofb": test_hal a inregistrat doar un fake-fb
        kprintf("[OK] test_drivers fb (nofb)\n");
        return;
    }
    FbInfo info;
    CHECK(dev->info(info));
    CHECK(info.width == 1024 && info.height == 768);
    kprintf("[OK] test_drivers fb 1024x768\n");
}

// Harness-ul vede markerul si trimite tastele; shell-ul tipareste raspunsul si pe serial
void check_input(){
    __asm__ volatile("sti");                    // ca in test_sched: intreruperile pot fi oprite pana aici
    shell_serial_echo = true;
    int before = shell_command_count;
    kprintf("drivers: astept intrare\n");
    uint64_t start = uptime_ms();
    while(shell_command_count == before && uptime_ms() - start < INPUT_TIMEOUT_MS){
        sleep_ms(20);
    }
    shell_serial_echo = false;
    CHECK(shell_command_count > before);
    kprintf("[OK] test_drivers input\n");
}

}

void test_drivers_run(){
    check_lspci();
    check_fb();
    check_input();
    kprintf("[OK] test_drivers (lspci, fb, input)\n");
}

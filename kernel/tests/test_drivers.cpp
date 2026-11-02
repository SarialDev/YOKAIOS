#include "tests/test_drivers.h"
#include "drivers/pci/pci.h"
#include "drivers/fb/fb.h"
#include "hal/fb.h"
#include "time/clock.h"
#include "drivers/tty.h"
#include "lib/kprintf.h"
#include "core/panic.h"

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

// Harness-ul vede markerul si tasteaza "version<ret>" prin monitorul QEMU; linia ajunge pe tty (modul canonic)
void check_input(){
    __asm__ volatile("sti");                    // ca in test_sched: intreruperile pot fi oprite pana aici
    tty_activate();
    kprintf("drivers: astept intrare\n");
    char line[64];
    uint64_t got = 0;
    uint64_t start = uptime_ms();
    bool done = false;
    while(!done && got < sizeof(line) - 1 && uptime_ms() - start < INPUT_TIMEOUT_MS){
        int64_t n = tty_read(line + got, sizeof(line) - 1 - got, true);
        if(n > 0){
            got += (uint64_t)n;
            done = line[got - 1] == '\n';
        } else {
            sleep_ms(20);
        }
    }
    line[got] = 0;
    CHECK(done && got == 8);
    CHECK(line[0] == 'v' && line[1] == 'e' && line[2] == 'r' && line[3] == 's' && line[4] == 'i' && line[5] == 'o' &&
          line[6] == 'n' && line[7] == '\n');
    kprintf("[OK] test_drivers input\n");
}

}

void test_drivers_run(){
    check_lspci();
    check_fb();
    check_input();
    kprintf("[OK] test_drivers (lspci, fb, input)\n");
}

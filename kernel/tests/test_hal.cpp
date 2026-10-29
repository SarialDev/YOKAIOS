#include "tests/test_hal.h"
#include "hal/hal.h"
#include "hal/console.h"
#include "hal/timer.h"
#include "hal/irqchip.h"
#include "hal/block.h"
#include "hal/net.h"
#include "hal/fb.h"
#include "hal/input.h"
#include "lib/kprintf.h"
#include "lib/string.h"
#include "core/panic.h"

namespace {

#define CHECK(cond) do { if(!(cond)) panic("test_hal: %s (linia %d)", #cond, __LINE__); } while(0)

class RamDisk final : public BlockDevice {
public:
    constexpr RamDisk() = default;
    uint32_t block_size() const override { return 512; }
    uint64_t block_count() const override { return 8; }
    int read(uint64_t lba, uint32_t count, void* buf) override {
        if(lba + count > 8){
            return -1;
        }
        memcpy(buf, data + lba * 512, count * 512);
        return 0;
    }
    int write(uint64_t lba, uint32_t count, const void* buf) override {
        if(lba + count > 8){
            return -1;
        }
        memcpy(data + lba * 512, buf, count * 512);
        return 0;
    }
private:
    uint8_t data[8 * 512] = {};
};

class LoopNet final : public NetDevice {
public:
    constexpr LoopNet() = default;
    void mac(uint8_t out[6]) const override { for(int i = 0; i < 6; i++){ out[i] = (uint8_t)(0x10 + i); } }
    uint32_t mtu() const override { return 1500; }
    bool link_up() override { return true; }
    int send(const void* frame, size_t len) override {
        if(len > sizeof(buf) || pending){
            return -1;
        }
        memcpy(buf, frame, len);
        pending = len;
        return 0;
    }
    int recv(void* out, size_t max) override {
        if(!pending){
            return 0;
        }
        size_t n = pending < max ? pending : max;
        memcpy(out, buf, n);
        pending = 0;
        return (int)n;
    }
private:
    uint8_t buf[64] = {};
    size_t pending = 0;
};

class FakeFb final : public Framebuffer {
public:
    constexpr FakeFb() = default;
    bool info(FbInfo& out) const override {
        out.addr = 0xB8000;
        out.width = 80;
        out.height = 25;
        out.pitch = 160;
        out.bpp = 16;
        return true;
    }
};

class FakeMouse final : public InputDevice {
public:
    constexpr FakeMouse() = default;
    InputKind kind() const override { return InputKind::Mouse; }
};

class FillerNet final : public NetDevice {
public:
    constexpr FillerNet() = default;
    void mac(uint8_t out[6]) const override { memset(out, 0, 6); }
    uint32_t mtu() const override { return 0; }
    bool link_up() override { return false; }
    int send(const void*, size_t) override { return -1; }
    int recv(void*, size_t) override { return -1; }
};

RamDisk ramdisk;
LoopNet loopnet;
FakeFb fakefb;
FakeMouse mouse;
FillerNet fillers[HAL_MAX_PER_KIND];

bool streq(const char* a, const char* b){
    if(!a || !b){
        return false;
    }
    while(*a && *a == *b){
        a++;
        b++;
    }
    return *a == *b;
}

void check_builtin(){
    CHECK(hal_count(HalKind::Console) >= 2);
    CHECK(streq(hal_name(HalKind::Console, 0), "vga"));
    CHECK(streq(hal_name(HalKind::Console, 1), "serial"));
    // Ecranul e VGA text, sau fbcon cand exista framebuffer (VGA tace atunci)
    Console* screen = hal_console_screen();
    CHECK(screen && (screen->flags() & CONSOLE_SCREEN));
    CHECK(screen == hal_console(0) || streq(hal_name(HalKind::Console, 2), "fbcon"));
    CHECK(hal_get(HalKind::Console, -1) == nullptr);
    CHECK(hal_get(HalKind::Console, hal_count(HalKind::Console)) == nullptr);
    CHECK(hal_get(HalKind::Count, 0) == nullptr);

    ClockSource* clk = hal_clock();
    CHECK(clk && streq(hal_name(HalKind::Clock, 0), "tsc"));
    CHECK(clk->freq_hz() > 1000000);
    uint64_t a = clk->read();
    CHECK(clk->read() >= a);

    InterruptController* chip = hal_irqchip();
    CHECK(chip);
    // Controlerul activ e cel mai recent inregistrat: APIC daca ACPI a gasit un LAPIC
    CHECK(hal_primary(HalKind::IrqChip) == (HalDevice*)chip);
    CHECK(chip->has(IRQCHIP_SMP) == streq(hal_name(HalKind::IrqChip, hal_count(HalKind::IrqChip) - 1), "apic"));
    CHECK(hal_count(HalKind::Input) >= 1);
    CHECK(hal_input(0)->kind() == InputKind::Keyboard);
}

void check_block(){
    CHECK(hal_register(HalKind::Block, &ramdisk, "ram0"));
    BlockDevice* b = hal_block(0);
    CHECK(b == &ramdisk && streq(hal_name(HalKind::Block, 0), "ram0"));
    uint8_t out[1024], in[1024];
    for(int i = 0; i < 1024; i++){
        out[i] = (uint8_t)(i * 7);
    }
    CHECK(b->write(2, 2, out) == 0);
    memset(in, 0, sizeof(in));
    CHECK(b->read(2, 2, in) == 0);
    CHECK(memcmp(in, out, sizeof(in)) == 0);
    CHECK(b->read(7, 2, in) < 0);
}

void check_net_fb(){
    CHECK(hal_register(HalKind::Net, &loopnet, "lo0"));
    NetDevice* n = hal_net(0);
    uint8_t mac[6];
    n->mac(mac);
    CHECK(mac[0] == 0x10 && mac[5] == 0x15 && n->link_up());
    const char msg[] = "ping";
    char buf[16];
    CHECK(n->recv(buf, sizeof(buf)) == 0);
    CHECK(n->send(msg, sizeof(msg)) == 0);
    CHECK(n->recv(buf, sizeof(buf)) == (int)sizeof(msg) && memcmp(buf, msg, sizeof(msg)) == 0);

    // Cu framebuffer real (fb0, din fbcon) fakefb vine dupa el
    int before = hal_count(HalKind::Framebuffer);
    CHECK(hal_register(HalKind::Framebuffer, &fakefb, "fake-fb"));
    FbInfo info;
    CHECK(hal_count(HalKind::Framebuffer) == before + 1);
    CHECK(hal_fb(before)->info(info) && info.width == 80 && info.bpp == 16);
}

void check_input_bus(){
    CHECK(hal_register(HalKind::Input, &mouse, "fake-mouse"));
    // Goleste ce a ramas de la tastatura si dezactiveaza abonatul (shell-ul) cat dureaza testul
    InputEvent ev;
    while(input_pop(ev)){
    }
    InputListener saved = input_subscribe(nullptr);
    uint64_t dropped0 = input_dropped();

    const uint32_t extra = 3;
    for(uint32_t i = 0; i < INPUT_RING_SIZE + extra; i++){
        InputEvent e;
        e.type = (i & 1) ? InputType::Mouse : InputType::Key;
        e.raw = 0;
        e.code = 0;
        e.value = (int32_t)i;
        e.time_ns = i;
        input_push(e);
    }
    CHECK(input_dropped() - dropped0 == extra);
    for(uint32_t i = extra; i < INPUT_RING_SIZE + extra; i++){
        CHECK(input_pop(ev));
        CHECK(ev.value == (int32_t)i);
        CHECK(ev.type == ((i & 1) ? InputType::Mouse : InputType::Key));
    }
    CHECK(!input_pop(ev));

    static volatile int seen = 0;
    input_subscribe([](const InputEvent& e){ seen = e.key.ascii; });
    InputEvent k;
    k.type = InputType::Key;
    k.raw = 0x1E;
    k.code = 0;
    k.value = 0;
    k.time_ns = 0;
    k.key = KeyEvent{KEY_A, 'A', true, MOD_SHIFT};
    input_push(k);
    CHECK(seen == 'A');
    CHECK(input_pop(ev) && ev.type == InputType::Key && ev.key.keycode == KEY_A && ev.key.modifiers == MOD_SHIFT);
    input_subscribe(saved);
}

void check_full(){
    // Registrul Net are deja lo0: restul locurilor se umplu, apoi inregistrarea esueaza
    int free_slots = HAL_MAX_PER_KIND - hal_count(HalKind::Net);
    for(int i = 0; i < free_slots; i++){
        CHECK(hal_register(HalKind::Net, &fillers[i], "filler"));
    }
    CHECK(!hal_register(HalKind::Net, &fillers[0], "extra"));
    CHECK(hal_count(HalKind::Net) == HAL_MAX_PER_KIND);
    CHECK(!hal_register(HalKind::Block, nullptr, "nul"));
    CHECK(!hal_register(HalKind::Block, &ramdisk, nullptr));
}

}

void test_hal_run(){
    check_builtin();
    check_block();
    check_net_fb();
    check_input_bus();
    check_full();
    kprintf("[OK] test_hal\n");
}

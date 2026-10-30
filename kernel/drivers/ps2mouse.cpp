#include "drivers/ps2mouse.h"
#include "drivers/i8042.h"
#include "arch/x86_64/pic.h"
#include "hal/irqchip.h"
#include "time/clock.h"
#include "boot/multiboot2.h"
#include "lib/kprintf.h"
#include "interrupt.h"
#include "io.h"

using namespace i8042;

namespace {

const uint8_t MOUSE_ACK = 0xFA;
const uint8_t MOUSE_RESEND = 0xFE;
const uint8_t MOUSE_RESET = 0xFF;
const uint8_t MOUSE_DEFAULTS = 0xF6;
const uint8_t MOUSE_SAMPLE_RATE = 0xF3;
const uint8_t MOUSE_GET_ID = 0xF2;
const uint8_t MOUSE_ENABLE_STREAM = 0xF4;
const uint8_t MOUSE_RESET_OK = 0xAA;
const uint8_t ID_INTELLIMOUSE = 3;

const uint8_t SAMPLE_RATE_DEFAULT = 100;
const uint8_t RETRIES = 3;
const uint32_t RESET_WAITS = 20;            // resetul dureaza pana la ~500 ms pe hardware real
const uint64_t PACKET_TIMEOUT_NS = 100000000;   // un pachet neterminat atat de mult se arunca

// Primul octet: 0 = L, 1 = R, 2 = M, 3 = mereu 1, 4/5 = semnul lui X/Y, 6/7 = overflow X/Y
const uint8_t B0_BUTTONS = 0x07;
const uint8_t B0_SYNC = 0x08;
const uint8_t B0_XSIGN = 0x10;
const uint8_t B0_YSIGN = 0x20;
const uint8_t B0_OVERFLOW = 0xC0;

class Ps2Mouse final : public InputDevice {
public:
    constexpr Ps2Mouse() = default;
    InputKind kind() const override { return InputKind::Mouse; }
};

Ps2Mouse mouse;
Ps2MouseDecoder decoder;
uint64_t last_byte_ns = 0;
bool debug_print = false;

// Citeste un octet de la portul auxiliar; false la timeout
bool read_byte(uint8_t& out, uint32_t waits = 1){
    for(uint32_t i = 0; i < waits; i++){
        if(wait_output()){
            out = inb(KBD_DATA);
            return true;
        }
    }
    return false;
}

// Trimite un octet mouse-ului si asteapta ACK; la 0xFE (resend) reincearca
bool mouse_cmd(uint8_t byte){
    for(uint8_t attempt = 0; attempt < RETRIES; attempt++){
        ctrl_cmd(CTRL_WRITE_P2);
        data_write(byte);
        uint8_t r;
        while(read_byte(r)){
            if(r == MOUSE_ACK){
                return true;
            }
            if(r == MOUSE_RESEND){
                break;
            }
        }
    }
    return false;
}

bool mouse_cmd_arg(uint8_t cmd, uint8_t arg){
    return mouse_cmd(cmd) && mouse_cmd(arg);
}

// Secventa magica 200, 100, 80 pentru rata de esantionare: un IntelliMouse raspunde apoi cu ID 3
bool probe_wheel(){
    const uint8_t magic[3] = {200, 100, 80};
    for(uint8_t rate : magic){
        if(!mouse_cmd_arg(MOUSE_SAMPLE_RATE, rate)){
            return false;
        }
    }
    uint8_t id;
    return mouse_cmd(MOUSE_GET_ID) && read_byte(id) && id == ID_INTELLIMOUSE;
}

// "debug=mouse" sau "debug=a,mouse,b" in cmdline
bool debug_enabled(){
    const char* v = cmdline_get("debug=");
    while(v && *v && *v != ' '){
        const char* n = "mouse";
        const char* p = v;
        while(*n && *p == *n){
            p++;
            n++;
        }
        if(!*n && (*p == ',' || *p == ' ' || *p == '\0')){
            return true;
        }
        while(*v && *v != ',' && *v != ' '){
            v++;
        }
        if(*v == ','){
            v++;
        }
    }
    return false;
}

void mouse_irq(InterruptFrame*){
    // Doar octetii de la portul 2; un octet de tastatura ramane pentru IRQ1
    for(;;){
        uint8_t status = inb(KBD_STATUS);
        if((status & (STATUS_OUTPUT_FULL | STATUS_AUX)) != (STATUS_OUTPUT_FULL | STATUS_AUX)){
            return;
        }
        uint8_t byte = inb(KBD_DATA);
        uint64_t now = monotonic_ns();
        if(now - last_byte_ns > PACKET_TIMEOUT_NS){
            decoder.reset();
        }
        last_byte_ns = now;

        MouseEvent m;
        if(!decoder.feed(byte, m)){
            continue;
        }
        InputEvent ev;
        ev.type = InputType::Mouse;
        ev.raw = byte;
        ev.code = 0;
        ev.value = 0;
        ev.time_ns = now;
        ev.mouse = m;
        input_push(ev);
        if(debug_print){
            kprintf("[mouse] dx=%d dy=%d dz=%d buttons=%x\n", (int)m.dx, (int)m.dy, (int)m.dz, (unsigned)m.buttons);
        }
    }
}

}

bool Ps2MouseDecoder::feed(uint8_t byte, MouseEvent& out){
    if(idx == 0 && !(byte & B0_SYNC)){
        return false;       // desincronizat: asteptam un prim octet valid
    }
    buf[idx++] = byte;
    if(idx < size){
        return false;
    }
    idx = 0;

    uint8_t b0 = buf[0];
    if(b0 & B0_OVERFLOW){
        return false;
    }
    // Deplasarea e pe 9 biti cu semn: bitul de semn e in primul octet
    int dx = buf[1] - ((b0 & B0_XSIGN) ? 256 : 0);
    int dy = buf[2] - ((b0 & B0_YSIGN) ? 256 : 0);
    out.dx = (int16_t)dx;
    out.dy = (int16_t)-dy;      // PS/2: pozitiv = sus; noi: pozitiv = jos
    out.dz = 0;
    if(size == 4){
        out.dz = (int16_t)(int8_t)buf[3];
    }
    out.buttons = 0;
    if(b0 & 0x01){
        out.buttons |= MOUSE_LEFT;
    }
    if(b0 & 0x02){
        out.buttons |= MOUSE_RIGHT;
    }
    if(b0 & 0x04){
        out.buttons |= MOUSE_MIDDLE;
    }
    return true;
}

void ps2mouse_init(){
    debug_print = debug_enabled();
    if(!present()){
        kprintf("ps2mouse: fara controler 8042\n");
        return;
    }

    // Tastatura oprita cat dureaza init-ul, ca sa nu-i furam octetii (si invers)
    ctrl_cmd(CTRL_DISABLE_P1);
    flush_output();
    ctrl_cmd(CTRL_ENABLE_P2);

    ctrl_cmd(CTRL_READ_CONFIG);
    if(!wait_output()){
        kprintf("[WARN] ps2mouse: nu citesc configuratia\n");
        ctrl_cmd(CTRL_ENABLE_P1);
        return;
    }
    uint8_t config = inb(KBD_DATA);
    config &= ~(CFG_IRQ12 | CFG_P2_CLOCK_OFF);   // P1 ramane oprit pana la sfarsit
    ctrl_cmd(CTRL_WRITE_CONFIG);
    data_write(config);
    flush_output();

    // Reset: ACK, apoi 0xAA (autotest) si ID. Lipsa raspunsului = nu e mouse conectat
    ctrl_cmd(CTRL_WRITE_P2);
    data_write(MOUSE_RESET);
    uint8_t r = 0;
    bool present = false;
    if(read_byte(r, RESET_WAITS) && r == MOUSE_ACK){
        present = read_byte(r, RESET_WAITS) && r == MOUSE_RESET_OK;
        if(present){
            read_byte(r, RESET_WAITS);      // ID 0x00
        }
    }
    flush_output();
    if(!present || !mouse_cmd(MOUSE_DEFAULTS)){
        kprintf("[WARN] ps2mouse: nu raspunde, ignorat\n");
        ctrl_cmd(CTRL_ENABLE_P1);
        return;
    }

    bool wheel = probe_wheel();
    if(!mouse_cmd_arg(MOUSE_SAMPLE_RATE, SAMPLE_RATE_DEFAULT) || !mouse_cmd(MOUSE_ENABLE_STREAM)){
        kprintf("[WARN] ps2mouse: pornirea streamingului nu a fost confirmata\n");
        ctrl_cmd(CTRL_ENABLE_P1);
        return;
    }
    decoder.set_wheel(wheel);
    flush_output();

    hal_register(HalKind::Input, &mouse, wheel ? "ps2-mouse (wheel)" : "ps2-mouse");
    irq_register(PIC_IRQ_BASE + MOUSE_IRQ, mouse_irq);
    ctrl_cmd(CTRL_WRITE_CONFIG);
    data_write(config | CFG_IRQ12);
    ctrl_cmd(CTRL_ENABLE_P1);
    InterruptController* chip = hal_irqchip();
    chip->route_irq(MOUSE_IRQ, PIC_IRQ_BASE + MOUSE_IRQ, chip->cpu_id());
    kprintf("ps2mouse: %s\n", wheel ? "IntelliMouse (roata)" : "standard");
}

#include "drivers/fb/fb.h"
#include "arch/x86_64/pat.h"
#include "arch/x86_64/paging.h"
#include "boot/multiboot2.h"
#include "mm/vmm.h"
#include "mm/pmm.h"
#include "lib/kprintf.h"
#include "init.h"

namespace {

// Framebuffer-ul se mapeaza in PML4[256] (cel al HHDM, deci comun tuturor spatiilor de adrese),
// la HHDM_BASE + 256 GiB + adresa fizica: dincolo de HHDM, care acopera doar RAM-ul
const uint64_t FB_VIRT_BASE = HHDM_BASE + (256UL << 30);
const uint64_t FB_PHYS_LIMIT = 256UL << 30;

struct State {
    uint8_t* front;
    uint8_t* back;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t shift[3];       // pozitiile R, G, B
    uint8_t size[3];        // marimile canalelor in biti
    bool wc;
    bool ready;
};

State st;

class FbDevice final : public Framebuffer {
public:
    constexpr FbDevice() = default;

    bool info(FbInfo& out) const override {
        if(!st.ready){
            return false;
        }
        out.addr = (uint64_t)st.front;
        out.width = st.width;
        out.height = st.height;
        out.pitch = st.pitch;
        out.bpp = 32;
        return true;
    }

    void flush() override { fb_flush(); }
};

FbDevice device;

inline void fill32(uint8_t* dst, uint32_t v, uint32_t n){
    uint64_t count = n;
    __asm__ volatile("rep stosl" : "+D"(dst), "+c"(count) : "a"(v) : "memory");
}

// rep movsq: de 8 ori mai putine iteratii decat movsb (emulatorul le plateste una cate una); doar sens crescator
void copy_up(void* dst, const void* src, uint64_t n){
    uint64_t q = n >> 3;
    __asm__ volatile("rep movsq" : "+D"(dst), "+S"(src), "+c"(q) : : "memory");
    n &= 7;
    __asm__ volatile("rep movsb" : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
}

inline uint32_t* px(uint8_t* base, int x, int y){
    return (uint32_t*)(base + (uint64_t)y * st.pitch + (uint64_t)x * 4);
}

}

__init bool fb_init(){
    if(st.ready){
        return true;
    }
    if(!boot_info.has_fb){
        return false;
    }
    const BootFramebuffer& b = boot_info.fb;
    if(b.type != 1 || b.bpp != 32 || b.width == 0 || b.height == 0 || b.pitch < b.width * 4){
        return false;
    }
    uint64_t bytes = (uint64_t)b.pitch * b.height;
    uint64_t phys = b.addr & ~(PAGE_4K - 1);
    uint64_t off = b.addr - phys;
    uint64_t pages = (off + bytes + PAGE_4K - 1) / PAGE_4K;
    if(phys + pages * PAGE_4K > FB_PHYS_LIMIT){
        return false;
    }

    bool wc = pat_init_cpu();
    // WC = intrarea PAT 1 (doar PWT); fara PAT, necache-uit (PWT | PCD = UC)
    uint64_t flags = PTE_RW | PTE_GLOBAL | PTE_NX | PTE_PWT | (wc ? 0 : PTE_PCD);
    for(uint64_t i = 0; i < pages; i++){
        if(!vmm_map(kernel_as, FB_VIRT_BASE + phys + i * PAGE_4K, phys + i * PAGE_4K, flags)){
            return false;
        }
    }

    st.front = (uint8_t*)(FB_VIRT_BASE + b.addr);
    st.width = b.width;
    st.height = b.height;
    st.pitch = b.pitch;
    st.shift[0] = b.red_pos;
    st.shift[1] = b.green_pos;
    st.shift[2] = b.blue_pos;
    st.size[0] = b.red_size;
    st.size[1] = b.green_size;
    st.size[2] = b.blue_size;
    st.wc = wc;
    if(!st.size[0] && !st.size[1] && !st.size[2]){
        // Tag fara informatii de culoare: xRGB, ca la VBE 32 bpp
        st.shift[0] = 16; st.shift[1] = 8; st.shift[2] = 0;
        st.size[0] = st.size[1] = st.size[2] = 8;
    }

    uint64_t back_pages = (bytes + PAGE_4K - 1) / PAGE_4K;
    uint64_t back_phys = pmm_alloc_contig(back_pages);
    st.back = back_phys ? (uint8_t*)phys_to_virt(back_phys) : nullptr;

    st.ready = true;
    fb_fill_rect(0, 0, (int)st.width, (int)st.height, 0);
    return true;
}

bool fb_ready(){ return st.ready; }
Framebuffer* fb_device(){ return st.ready ? &device : nullptr; }
uint32_t fb_width(){ return st.width; }
uint32_t fb_height(){ return st.height; }
bool fb_write_combining(){ return st.wc; }
bool fb_has_backbuffer(){ return st.back != nullptr; }

uint32_t fb_rgb(uint8_t r, uint8_t g, uint8_t b){
    const uint8_t c[3] = {r, g, b};
    uint32_t v = 0;
    for(int i = 0; i < 3; i++){
        v |= (uint32_t)(c[i] >> (8 - st.size[i])) << st.shift[i];
    }
    return v;
}

uint32_t fb_color(uint32_t rgb){
    return fb_rgb((uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb);
}

void fb_putpixel(int x, int y, uint32_t color){
    if(!st.ready || x < 0 || y < 0 || x >= (int)st.width || y >= (int)st.height){
        return;
    }
    *px(st.front, x, y) = color;
    if(st.back){
        *px(st.back, x, y) = color;
    }
}

uint32_t fb_getpixel(int x, int y){
    if(!st.ready || x < 0 || y < 0 || x >= (int)st.width || y >= (int)st.height){
        return 0;
    }
    return *px(st.back ? st.back : st.front, x, y);
}

void fb_fill_rect(int x, int y, int w, int h, uint32_t color){
    if(!st.ready){
        return;
    }
    int x1 = x + w;
    int y1 = y + h;
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x1 > (int)st.width) x1 = (int)st.width;
    if(y1 > (int)st.height) y1 = (int)st.height;
    if(x >= x1 || y >= y1){
        return;
    }
    for(int row = y; row < y1; row++){
        fill32((uint8_t*)px(st.front, x, row), color, (uint32_t)(x1 - x));
        if(st.back){
            fill32((uint8_t*)px(st.back, x, row), color, (uint32_t)(x1 - x));
        }
    }
}

void fb_blit(int x, int y, const uint32_t* src, int w, int h, uint32_t src_pitch){
    if(!st.ready || !src){
        return;
    }
    int sx = 0;
    int sy = 0;
    if(x < 0){ sx = -x; x = 0; }
    if(y < 0){ sy = -y; y = 0; }
    int cw = w - sx;
    int ch = h - sy;
    if(cw > (int)st.width - x) cw = (int)st.width - x;
    if(ch > (int)st.height - y) ch = (int)st.height - y;
    if(cw <= 0 || ch <= 0){
        return;
    }
    const uint8_t* s = (const uint8_t*)src + (uint64_t)sy * src_pitch + (uint64_t)sx * 4;
    for(int row = 0; row < ch; row++, s += src_pitch){
        copy_up(px(st.front, x, y + row), s, (uint64_t)cw * 4);
        if(st.back){
            copy_up(px(st.back, x, y + row), s, (uint64_t)cw * 4);
        }
    }
}

void fb_draw_glyph(int x, int y, const uint8_t* rows, int height, uint32_t fg, uint32_t bg){
    if(!st.ready || x < 0 || y < 0 || x + 8 > (int)st.width || y + height > (int)st.height){
        return;
    }
    for(int r = 0; r < height; r++){
        uint8_t bits = rows[r];
        uint32_t* f = px(st.front, x, y + r);
        uint32_t* b = st.back ? px(st.back, x, y + r) : nullptr;
        for(int c = 0; c < 8; c++){
            uint32_t v = (bits & (0x80 >> c)) ? fg : bg;
            f[c] = v;
            if(b){
                b[c] = v;
            }
        }
    }
}

void fb_scroll_up(int dy, uint32_t fill){
    if(!st.ready || dy <= 0){
        return;
    }
    if(dy >= (int)st.height){
        fb_fill_rect(0, 0, (int)st.width, (int)st.height, fill);
        return;
    }
    uint64_t keep = (uint64_t)(st.height - dy) * st.pitch;
    uint64_t shift = (uint64_t)dy * st.pitch;
    if(st.back){
        copy_up(st.back, st.back + shift, keep);
        for(uint32_t row = st.height - dy; row < st.height; row++){
            fill32(st.back + (uint64_t)row * st.pitch, fill, st.width);
        }
        copy_up(st.front, st.back, (uint64_t)st.height * st.pitch);
    } else {
        copy_up(st.front, st.front + shift, keep);
        for(uint32_t row = st.height - dy; row < st.height; row++){
            fill32(st.front + (uint64_t)row * st.pitch, fill, st.width);
        }
    }
}

void fb_flush(){
    if(st.ready && st.back){
        copy_up(st.front, st.back, (uint64_t)st.height * st.pitch);
    }
}

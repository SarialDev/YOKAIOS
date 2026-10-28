#pragma once
#include "hal/hal.h"

struct FbInfo {
    uint64_t addr;      // adresa virtuala a bufferului
    uint32_t width;
    uint32_t height;
    uint32_t pitch;     // bytes pe linie
    uint8_t bpp;
};

class Framebuffer : public HalDevice {
public:
    virtual bool info(FbInfo& out) const = 0;
    // Pentru buffere cu dublu buffering sau scriere indirecta; implicit nu face nimic
    virtual void flush() {}
protected:
    constexpr Framebuffer() = default;
};

inline Framebuffer* hal_fb(int idx){
    return static_cast<Framebuffer*>(hal_get(HalKind::Framebuffer, idx));
}

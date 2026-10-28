#pragma once
#include "hal/hal.h"

class NetDevice : public HalDevice {
public:
    virtual void mac(uint8_t out[6]) const = 0;
    virtual uint32_t mtu() const = 0;
    virtual bool link_up() = 0;
    // 0 la succes, negativ la eroare (ex. coada plina)
    virtual int send(const void* frame, size_t len) = 0;
    // Lungimea cadrului primit, 0 daca nu e niciunul, negativ la eroare
    virtual int recv(void* buf, size_t max) = 0;
protected:
    constexpr NetDevice() = default;
};

inline NetDevice* hal_net(int idx){
    return static_cast<NetDevice*>(hal_get(HalKind::Net, idx));
}

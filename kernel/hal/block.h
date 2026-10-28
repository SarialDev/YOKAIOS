#pragma once
#include "hal/hal.h"

class BlockDevice : public HalDevice {
public:
    virtual uint32_t block_size() const = 0;
    virtual uint64_t block_count() const = 0;
    // 0 la succes, negativ la eroare
    virtual int read(uint64_t lba, uint32_t count, void* buf) = 0;
    virtual int write(uint64_t lba, uint32_t count, const void* buf) = 0;
    virtual int flush() { return 0; }
protected:
    constexpr BlockDevice() = default;
};

inline BlockDevice* hal_block(int idx){
    return static_cast<BlockDevice*>(hal_get(HalKind::Block, idx));
}

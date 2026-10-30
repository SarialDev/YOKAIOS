#include "lib/crc.h"

namespace {

uint32_t tab32[256];
uint16_t tab16[256];
volatile bool ready = false;

void init_tables(){
    for(uint32_t i = 0; i < 256; i++){
        uint32_t c = i;
        uint16_t d = (uint16_t)i;
        for(int k = 0; k < 8; k++){
            c = (c & 1) ? 0x82F63B78u ^ (c >> 1) : c >> 1;
            d = (d & 1) ? (uint16_t)(0xA001u ^ (d >> 1)) : (uint16_t)(d >> 1);
        }
        tab32[i] = c;
        tab16[i] = d;
    }
    __atomic_store_n(&ready, true, __ATOMIC_RELEASE);
}

}

uint32_t crc32c(uint32_t crc, const void* data, uint64_t len){
    if(!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)){
        init_tables();                  // idempotent: acelasi continut scris de oricine
    }
    const uint8_t* p = (const uint8_t*)data;
    while(len--){
        crc = tab32[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    }
    return crc;
}

uint16_t crc16(uint16_t crc, const void* data, uint64_t len){
    if(!__atomic_load_n(&ready, __ATOMIC_ACQUIRE)){
        init_tables();
    }
    const uint8_t* p = (const uint8_t*)data;
    while(len--){
        crc = (uint16_t)(tab16[(crc ^ *p++) & 0xFF] ^ (crc >> 8));
    }
    return crc;
}

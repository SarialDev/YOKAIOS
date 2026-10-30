#pragma once
#include "types.h"
#include "io.h"

// Controlerul PS/2 8042, comun tastaturii (port 1) si mouse-ului (port 2). Doar acces prin polling, folosit la init
namespace i8042 {

const uint16_t KBD_DATA = 0x60;
const uint16_t KBD_STATUS = 0x64;       // la citire: status, la scriere: comanda pentru controler
const uint8_t STATUS_OUTPUT_FULL = 0x01;
const uint8_t STATUS_INPUT_FULL = 0x02;
const uint8_t STATUS_AUX = 0x20;        // octetul din bufferul de iesire vine de la port 2 (mouse)
const uint32_t WAIT_LOOPS = 100000;

const uint8_t CTRL_READ_CONFIG = 0x20;
const uint8_t CTRL_WRITE_CONFIG = 0x60;
const uint8_t CTRL_DISABLE_P2 = 0xA7;
const uint8_t CTRL_ENABLE_P2 = 0xA8;
const uint8_t CTRL_P2_TEST = 0xA9;
const uint8_t CTRL_ENABLE_P1 = 0xAE;
const uint8_t CTRL_DISABLE_P1 = 0xAD;
const uint8_t CTRL_SELF_TEST = 0xAA;
const uint8_t CTRL_P1_TEST = 0xAB;
const uint8_t CTRL_WRITE_P2 = 0xD4;     // urmatorul octet de date merge la dispozitivul de pe port 2
const uint8_t SELF_TEST_OK = 0x55;

const uint8_t CFG_IRQ1 = 0x01;
const uint8_t CFG_IRQ12 = 0x02;
const uint8_t CFG_P1_CLOCK_OFF = 0x10;
const uint8_t CFG_P2_CLOCK_OFF = 0x20;
const uint8_t CFG_TRANSLATE = 0x40;     // oprit: tastatura trimite set 1 direct

// Asteapta pana controlerul poate primi un octet; false la timeout
inline bool wait_input_free(){
    for(uint32_t i = 0; i < WAIT_LOOPS; i++){
        if(!(inb(KBD_STATUS) & STATUS_INPUT_FULL)){
            return true;
        }
    }
    return false;
}

// Asteapta un octet de la controler; false la timeout
inline bool wait_output(){
    for(uint32_t i = 0; i < WAIT_LOOPS; i++){
        if(inb(KBD_STATUS) & STATUS_OUTPUT_FULL){
            return true;
        }
    }
    return false;
}

// Fara controler (ex. QEMU -machine i8042=off, PC-uri doar cu USB) portul de status citeste 0xFF
inline bool present(){
    return inb(KBD_STATUS) != 0xFF;
}

// Limitat: fara controler bitul "output full" ar ramane mereu setat
inline void flush_output(){
    for(int i = 0; i < 64 && (inb(KBD_STATUS) & STATUS_OUTPUT_FULL); i++){
        inb(KBD_DATA);
    }
}

inline void ctrl_cmd(uint8_t cmd){
    wait_input_free();
    outb(KBD_STATUS, cmd);
}

inline void data_write(uint8_t byte){
    wait_input_free();
    outb(KBD_DATA, byte);
}

}

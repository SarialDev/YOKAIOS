#pragma once

// Decodoarele HID boot (tastatura: apasari/eliberari/modificatori/Caps/rollover; mouse) si, daca exista xHCI
// cu usb-kbd + usb-mouse (QEMU), prezenta lor in HAL; cmdline "test=usb"
void test_usb_run();

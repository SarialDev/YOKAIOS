#pragma once

// Semnaturi Ed25519 (T8.5): modulul semnat /lib/modules/hello_mod.ko se incarca si se descarca; modificat cu un octet
// (ELF, semnatura sau magic), nesemnat, trunchiat sau semnat cu alta cheie e refuzat; domeniile de semnare sunt separate.
// Pachetele (ypkg) se testeaza in user/test/ypkgtest.c; cmdline "test=sign"
void test_sign_run();

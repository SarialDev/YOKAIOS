#pragma once

// Discuri criptate (T8.8), cmdline "test=crypt" (de obicei cu DISK=<imagine> facuta de tests/check_crypt.sh):
//  1. vectori AES (FIPS 197), AES-XTS (calculat de o implementare independenta), ChaCha20 (RFC 8439) + AES-NI == software;
//  2. un disc in RAM (ram0) formatat / deschis / inchis / reparolat cu /bin/cryptsetup, pentru ambele cifruri: datele brute
//     de pe ram0 nu contin textul scris, parola gresita se refuza, volumul nu se inchide cat e montat;
//  3. ext4 deja existent pe sda (criptat pe gazda de tools/cryptimg): deschis cu cryptsetup, montat rw, citit, scris, demontat,
//     inchis, redeschis si regasit
void test_crypt_run();

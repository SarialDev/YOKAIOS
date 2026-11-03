#pragma once
#include "types.h"
#include "hal/block.h"
#include "uapi/crypt.h"

// Volume criptate peste un dispozitiv bloc (T8.8). cryptN e un BlockDevice virtual inregistrat in HAL (deci apare
// in /dev, lsblk, mount "cryptN"): fiecare sector de 512 de octeti al lui se scrie criptat pe dispozitivul parinte
// (disc sau partitie), dupa un antet de 4 KiB cu formatul din lib/crypto/ykcrypt.h (salt, cheie master impachetata cu o
// cheie derivata PBKDF2-HMAC-SHA256 din parola, MAC).
//
// Cifruri:
//  - aes-xts-plain64 (AES-256-XTS, tweak = numarul sectorului): cifrul normal. AES-NI daca CPU-ul il are, altfel
//    varianta software bitsliced in timp constant (lib/crypto/aes.c);
//  - chacha20-plain64: ChaCha20 cu nonce = numarul sectorului. E un cifru de flux: scrierea repetata a aceluiasi sector
//    foloseste acelasi flux, deci cine vede doua versiuni ale sectorului afla XOR-ul textelor in clar (XTS nu are
//    problema asta), si nimic nu e autentificat. Util ca alternativa fara AES (CPU-uri fara AES-NI), nu ca alegere implicita.
// Niciun cifru nu autentifica datele: un atacator cu acces la disc poate corupe sectoare (XTS: bloc de 16 octeti
// ilizibil, ChaCha20: schimbare bit cu bit) fara sa fie observat. Fara TPM / cheie in hardware.
//
// Deschiderea/inchiderea trec prin syscall-ul cryptctl (uapi/crypt.h), folosit de /sbin/cryptsetup. Cheia master ramane
// numai in kernel si se sterge la close. Sloturile sunt statice (CRYPT_SLOTS): dupa close, cryptN ramane in HAL ca
// dispozitiv inactiv (-ENODEV) si poate fi redeschis; nu se poate sterge un nod din registru.
void crypt_init();
int64_t sys_cryptctl(uint64_t op, uint64_t arg, uint64_t, uint64_t, uint64_t, uint64_t);

// Pentru teste din kernel: deschide cu parola data peste dev; name primeste "cryptN". 0 sau -errno
int crypt_open_kernel(const char* dev, const void* pass, uint32_t passlen, uint32_t flags, char* name);
int crypt_close_kernel(const char* name);

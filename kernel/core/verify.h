#pragma once
#include "types.h"

// Semnaturi Ed25519 (T8.5). Cheia publica este compilata in kernel (build/yokai_pubkey.c, generata de tools/sign.py din
// cheia privata, care ramane in afara depozitului). Mesajul semnat este mereu <context de domeniu> || date, deci o
// semnatura de pachet nu valoreaza ca semnatura de modul sau de kernel.
enum class SignKind : int {
    Package = 0,        // manifest.toml cu linia `signature = ...` inlocuita prin `signature = ""`
    Module = 1,         // imaginea ELF a unui modul de kernel (fara coada de semnatura)
    Kernel = 2,         // kernel.bin (semnatura detasata, verificata pe gazda; nimic din kernel nu se verifica pe sine)
};

const uint64_t SIG_SIZE = 64;
const uint64_t SIG_MODULE_MAGIC_SIZE = 8;     // un modul semnat = ELF || semnatura (64) || "~YKSIG~\n"
const uint64_t SIG_VERIFY_MAX = 2UL << 20;    // sigverify: cel mult 2 MiB de date

// Cheia publica de incredere (32 de octeti)
const uint8_t* sign_pubkey();
// Semnatura sig (64 de octeti) peste ctx(kind) || data este valida cu cheia kernelului?
bool sign_verify(SignKind kind, const void* data, uint64_t len, const uint8_t* sig);
// cmdline "sign=permissive": ypkg -U poate instala pachete NESEMNATE (pentru teste); semnaturile gresite se refuza mereu
bool sign_permissive();
// Imagine de modul semnata -> lungimea ELF-ului (fara coada) daca semnatura e valida; altfel -ENOKEY (fara coada) sau
// -EKEYREJECTED (semnatura gresita)
int sign_check_module(const void* blob, uint64_t len, uint64_t& elf_len);

// sigverify(op, date, lungime, semnatura):
//   op 0 (SIGV_PACKAGE): semnatura (64 de octeti, din user) peste manifestul din `date`; 0 sau -EKEYREJECTED
//   op 1 (SIGV_POLICY): intoarce bitii politicii: 1 = permisiv (vezi sign_permissive)
int64_t sys_sigverify(uint64_t op, uint64_t data, uint64_t len, uint64_t sig, uint64_t, uint64_t);

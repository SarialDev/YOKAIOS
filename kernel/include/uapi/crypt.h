#pragma once
/* cryptctl(op, arg): volume criptate (kernel/block/crypt.h), comune kernel / userland. Cere CAP_SYS_ADMIN.
 *  CRYPT_OPEN : arg = struct crypt_open_req*   deschide dev cu parola; kernelul deriva cheia, citeste antetul YKCRYPT1 si
 *               inregistreaza dispozitivul bloc "cryptN" (N in name la intoarcere; daca name e "cryptN" cere acel N)
 *  CRYPT_CLOSE: arg = nume "cryptN" (sir terminat cu 0)   inchide volumul; -EBUSY daca e montat sau parintele nu mai raspunde
 *  CRYPT_INFO : arg = struct crypt_info*  (name = intrare "cryptN"; name gol = primul activ dupa cel numit, vezi cryptsetup status) */
#define CRYPT_OP_OPEN    0
#define CRYPT_OP_CLOSE   1
#define CRYPT_OP_INFO    2

#define CRYPT_PASS_MAX   256
#define CRYPT_NAME_MAX   16
#define CRYPT_DEV_MAX    32
#define CRYPT_SLOTS      4

#define CRYPT_FLAG_FORCE_SOFT 1     /* numai AES software (timp constant), chiar daca exista AES-NI */

struct crypt_open_req {
    char dev[CRYPT_DEV_MAX];        /* "sda1" sau "/dev/sda1" */
    char name[CRYPT_NAME_MAX];      /* in: "" sau "cryptN"; out: numele dat */
    unsigned int passlen;
    unsigned int flags;
    unsigned char pass[CRYPT_PASS_MAX];
};

struct crypt_info {
    char name[CRYPT_NAME_MAX];      /* in: cryptN; out la fel */
    char dev[CRYPT_DEV_MAX];        /* dispozitivul parinte */
    unsigned int cipher;            /* 1 = aes-xts-plain64, 2 = chacha20-plain64 */
    unsigned int hw_aes;            /* 1 = AES-NI folosit */
    unsigned long long sectors;     /* de 512 octeti, vazute de cel care foloseste volumul */
    unsigned long long offset;      /* sectoare de antet pe parinte */
    unsigned char uuid[16];
};

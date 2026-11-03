/* cryptimg: unealta de gazda pentru volume YKCRYPT1 (lib/crypto/ykcrypt.h), cu ACELEASI surse lib/crypto ca kernelul.
 * Folosita de tests/check_crypt.sh: cripteaza o imagine (ext4 facut cu mke2fs -d) ca sa o monteze kernelul prin cryptsetup
 * si decripteaza discul dupa test ca sa-l verifice cu e2fsck / debugfs.
 *   cryptimg encrypt <clar.img> <criptat.img> <parola> <xts|chacha20> [iteratii]
 *   cryptimg decrypt <criptat.img> <clar.img> <parola>
 *   cryptimg sector  <criptat.img> <parola> <sector>      (hex al sectorului decriptat, pentru verificari incrucisate)
 * Compilare: gcc -O2 -Ilib tools/cryptimg.c lib/crypto/{aes,xts,chacha20,sha256,hmac,pbkdf2,ykcrypt}.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <crypto/ykcrypt.h>
#include <crypto/xts.h>
#include <crypto/chacha20.h>

static void die(const char *m){
    fprintf(stderr, "cryptimg: %s\n", m);
    exit(1);
}

static void random_bytes(uint8_t *b, size_t n){
    FILE *f = fopen("/dev/urandom", "rb");
    if(!f || fread(b, 1, n, f) != n){
        die("/dev/urandom");
    }
    fclose(f);
}

static void sector_crypt(const uint8_t *master, uint32_t cipher, int dec, uint64_t s, uint8_t *buf){
    if(cipher == YKC_AES_XTS){
        xts_ctx x;
        if(xts_set_key(&x, master, 64)){
            die("cheie XTS");
        }
        if(dec){
            xts_decrypt(&x, s, buf, buf, 512);
        } else {
            xts_encrypt(&x, s, buf, buf, 512);
        }
    } else {
        uint8_t nonce[12] = {0};
        for(int k = 0; k < 8; k++){
            nonce[k] = (uint8_t)(s >> (8 * k));
        }
        chacha20_xor(master, 0, nonce, buf, buf, 512);
    }
}

static void read_hdr(FILE *f, const char *pw, uint8_t *hdr, uint8_t *master, struct ykc_info *in){
    if(fread(hdr, 1, YKC_HDR_SIZE, f) != YKC_HDR_SIZE || ykc_parse(hdr, in)){
        die("antet YKCRYPT1 invalid");
    }
    if(ykc_unlock(hdr, pw, strlen(pw), master)){
        die("parola gresita");
    }
}

int main(int argc, char **argv){
    uint8_t hdr[YKC_HDR_SIZE], master[YKC_MASTER_LEN], buf[512];
    struct ykc_info in;
    if(argc >= 6 && strcmp(argv[1], "encrypt") == 0){
        FILE *p = fopen(argv[2], "rb"), *o = fopen(argv[3], "wb");
        if(!p || !o){
            die("nu pot deschide fisierele");
        }
        fseek(p, 0, SEEK_END);
        long size = ftell(p);
        fseek(p, 0, SEEK_SET);
        uint32_t cipher = strcmp(argv[5], "chacha20") == 0 ? YKC_CHACHA20 : YKC_AES_XTS;
        uint32_t iter = argc > 6 ? (uint32_t)strtoul(argv[6], NULL, 10) : 1000;
        uint8_t rnd[YKC_RND_LEN];
        random_bytes(rnd, sizeof(rnd));
        if(ykc_format(hdr, argv[4], strlen(argv[4]), cipher, iter, (uint64_t)size + YKC_HDR_SIZE, rnd, master)){
            die("parametri invalizi");
        }
        fwrite(hdr, 1, YKC_HDR_SIZE, o);
        for(long s = 0; s * 512 < size; s++){
            memset(buf, 0, 512);
            if(fread(buf, 1, 512, p) == 0){
                break;
            }
            sector_crypt(master, cipher, 0, (uint64_t)s, buf);
            fwrite(buf, 1, 512, o);
        }
        fclose(o);
        return 0;
    }
    if(argc == 5 && strcmp(argv[1], "decrypt") == 0){
        FILE *e = fopen(argv[2], "rb"), *o = fopen(argv[3], "wb");
        if(!e || !o){
            die("nu pot deschide fisierele");
        }
        read_hdr(e, argv[4], hdr, master, &in);
        for(uint64_t s = 0; s < in.payload_bytes / 512; s++){
            if(fread(buf, 1, 512, e) != 512){
                die("imagine trunchiata");
            }
            sector_crypt(master, in.cipher, 1, s, buf);
            fwrite(buf, 1, 512, o);
        }
        fclose(o);
        return 0;
    }
    if(argc == 5 && strcmp(argv[1], "sector") == 0){
        FILE *e = fopen(argv[2], "rb");
        if(!e){
            die("nu pot deschide imaginea");
        }
        read_hdr(e, argv[3], hdr, master, &in);
        uint64_t s = strtoull(argv[4], NULL, 10);
        fseek(e, (long)(YKC_HDR_SIZE + s * 512), SEEK_SET);
        if(fread(buf, 1, 512, e) != 512){
            die("sector in afara imaginii");
        }
        sector_crypt(master, in.cipher, 1, s, buf);
        for(int i = 0; i < 512; i++){
            printf("%02x", buf[i]);
        }
        printf("\n");
        return 0;
    }
    fprintf(stderr, "uz: cryptimg encrypt|decrypt|sector ... (vezi comentariul)\n");
    return 1;
}

/* cryptsetup: volume criptate (kernel/block/crypt.h, formatul antetului in lib/crypto/ykcrypt.h). Doar root (CAP_SYS_ADMIN).
 *   cryptsetup format [-c xts|chacha20] [-i iteratii] [-p parola | -k fisier-cheie] [-y] [--insecure-rng] <dispozitiv>
 *       scrie antetul YKCRYPT1 (4 KiB) la inceputul dispozitivului: SALT + cheia master aleatoare impachetata cu o cheie derivata
 *       PBKDF2-HMAC-SHA256 din parola. Datele existente NU se muta si nu se pastreaza (volumul incepe cu ce se afla acolo, decriptat
 *       = zgomot); scrie-le dupa open. Cere confirmarea "YES" daca nu e -y
 *   cryptsetup open [-p parola | -k fisier-cheie] [--soft] <dispozitiv> [cryptN]   deschide -> /dev/cryptN (se tipareste numele)
 *   cryptsetup close <cryptN>                                                      inchide (refuza daca e montat)
 *   cryptsetup status <cryptN>                                                     cifru, parinte, marime, AES-NI
 *   cryptsetup dump <dispozitiv>                                                   campurile antetului (fara secrete)
 *   cryptsetup passwd [-p parola-veche] [-n parola-noua] [-i iteratii] <dispozitiv>   schimba parola (aceeasi cheie master)
 * Parola: -p (vizibila in lista de procese; mai ales pentru teste), -k (fisier: parola = SHA-256 hex al continutului) sau se
 * citeste de la terminal fara ecou. -k si -p nu se combina. */
#include "authdb.h"
#include <crypto/ykcrypt.h>
#include <crypto/sha256.h>
#include <uapi/crypt.h>

#define PASS_BUF 512

long cryptctl(int op, void *arg, long a2, long a3);

static const char *pw_opt, *pw_new, *key_opt;

static void usage(void){
    fprintf(stderr, "uz: cryptsetup format|open|close|status|dump|passwd ... (vezi comentariul din user/bin/cryptsetup.c)\n");
}

static int hex_of(const char *path, char *out){
    char buf[KEY_MAX + 1];
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        fprintf(stderr, "cryptsetup: %s: %s\n", path, strerror(errno));
        return -1;
    }
    size_t len = 0;
    for(;;){
        ssize_t r = read(fd, buf + len, sizeof(buf) - len);
        if(r < 0){
            close(fd);
            fprintf(stderr, "cryptsetup: %s: eroare de citire\n", path);
            return -1;
        }
        if(r == 0){
            break;
        }
        len += (size_t)r;
        if(len > KEY_MAX){
            close(fd);
            fprintf(stderr, "cryptsetup: %s: fisier-cheie prea mare\n", path);
            return -1;
        }
    }
    close(fd);
    uint8_t h[32];
    sha256(buf, len, h);
    for(int i = 0; i < 32; i++){
        sprintf(out + 2 * i, "%02x", h[i]);
    }
    crypto_wipe(buf, sizeof(buf));
    return 0;
}

/* Parola in out (cel mult CRYPT_PASS_MAX octeti); lungimea sau -1 */
static int get_pass(const char *given, const char *keyfile, const char *prompt, int confirm, char *out){
    if(given && keyfile){
        fprintf(stderr, "cryptsetup: -p si -k nu se combina\n");
        return -1;
    }
    if(keyfile){
        return hex_of(keyfile, out) ? -1 : 64;
    }
    if(given){
        size_t n = strlen(given);
        if(n == 0 || n > CRYPT_PASS_MAX){
            fprintf(stderr, "cryptsetup: parola trebuie sa aiba 1..%d octeti\n", CRYPT_PASS_MAX);
            return -1;
        }
        memcpy(out, given, n);
        out[n] = 0;
        return (int)n;
    }
    char again[PASS_BUF];
    int n = adb_read_line(prompt, 0, out, PASS_BUF);
    if(n <= 0 || n > CRYPT_PASS_MAX){
        fprintf(stderr, "cryptsetup: parola trebuie sa aiba 1..%d octeti\n", CRYPT_PASS_MAX);
        return -1;
    }
    if(confirm){
        int m = adb_read_line("Repeta parola: ", 0, again, sizeof(again));
        if(m != n || memcmp(out, again, (size_t)n) != 0){
            fprintf(stderr, "cryptsetup: parolele difera\n");
            crypto_wipe(again, sizeof(again));
            return -1;
        }
        crypto_wipe(again, sizeof(again));
    }
    return n;
}

static const char *cipher_name(unsigned c){
    return c == YKC_AES_XTS ? "aes-xts-plain64" : c == YKC_CHACHA20 ? "chacha20-plain64" : "?";
}

static int has_rdrand(void){
    unsigned a = 1, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    return (c >> 30) & 1;
}

static int read_all(int fd, void *buf, size_t n){
    size_t got = 0;
    while(got < n){
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if(r <= 0){
            return -1;
        }
        got += (size_t)r;
    }
    return 0;
}

static int write_all(int fd, const void *buf, size_t n){
    size_t put = 0;
    while(put < n){
        ssize_t r = write(fd, (const char *)buf + put, n - put);
        if(r <= 0){
            return -1;
        }
        put += (size_t)r;
    }
    return 0;
}

static int load_hdr(const char *dev, uint8_t *hdr){
    int fd = open(dev, O_RDONLY);
    if(fd < 0){
        fprintf(stderr, "cryptsetup: %s: %s\n", dev, strerror(errno));
        return -1;
    }
    int rc = read_all(fd, hdr, YKC_HDR_SIZE);
    close(fd);
    if(rc){
        fprintf(stderr, "cryptsetup: %s: nu pot citi antetul\n", dev);
    }
    return rc;
}

static int cmd_format(const char *dev, unsigned cipher, unsigned iter, int yes, int weak_ok){
    if(!has_rdrand() && !weak_ok){
        fprintf(stderr, "cryptsetup: CPU-ul nu are RDRAND, deci /dev/random e doar TSC amestecat (previzibil): cheia master nu ar fi sigura.\n"
                        "            Formateaza pe hardware cu RDRAND sau da --insecure-rng (doar pentru teste)\n");
        return 1;
    }
    char pw[PASS_BUF];
    int n = get_pass(pw_opt, key_opt, "Parola noua: ", 1, pw);
    if(n < 0){
        return 1;
    }
    int fd = open(dev, O_RDWR);
    if(fd < 0){
        fprintf(stderr, "cryptsetup: %s: %s\n", dev, strerror(errno));
        return 1;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    if(size < 0 || lseek(fd, 0, SEEK_SET) < 0){
        fprintf(stderr, "cryptsetup: %s: nu pot afla marimea\n", dev);
        return 1;
    }
    if(!yes){
        char ans[16];
        printf("ATENTIE: antetul de pe %s se suprascrie; datele de acolo nu mai pot fi citite.\n", dev);
        if(adb_read_line("Scrie YES ca sa continui: ", 1, ans, sizeof(ans)) < 0 || strcmp(ans, "YES") != 0){
            fprintf(stderr, "cryptsetup: anulat\n");
            return 1;
        }
    }
    uint8_t rnd[YKC_RND_LEN];
    if(adb_random(rnd, sizeof(rnd))){
        fprintf(stderr, "cryptsetup: /dev/random indisponibil\n");
        return 1;
    }
    static uint8_t hdr[YKC_HDR_SIZE];
    if(ykc_format(hdr, pw, (size_t)n, cipher, iter, (uint64_t)size, rnd, NULL)){
        fprintf(stderr, "cryptsetup: parametri invalizi (dispozitiv prea mic? iteratii %d..%d)\n", YKC_ITER_MIN, YKC_ITER_MAX);
        return 1;
    }
    crypto_wipe(rnd, sizeof(rnd));
    crypto_wipe(pw, sizeof(pw));
    if(write_all(fd, hdr, YKC_HDR_SIZE)){
        fprintf(stderr, "cryptsetup: scrierea antetului pe %s a esuat\n", dev);
        return 1;
    }
    close(fd);
    printf("cryptsetup: %s formatat (%s, %u iteratii, %lu octeti utili)\n", dev, cipher_name(cipher), iter,
           (unsigned long)(((uint64_t)size - YKC_HDR_SIZE) & ~(uint64_t)(YKC_SECTOR - 1)));
    return 0;
}

static int cmd_open(const char *dev, const char *want, int soft){
    struct crypt_open_req *r = calloc(1, sizeof(*r));
    char pw[PASS_BUF];
    if(!r){
        return 1;
    }
    int n = get_pass(pw_opt, key_opt, "Parola: ", 0, pw);
    if(n < 0){
        return 1;
    }
    snprintf(r->dev, sizeof(r->dev), "%s", dev);
    if(want){
        snprintf(r->name, sizeof(r->name), "%s", want);
    }
    memcpy(r->pass, pw, (size_t)n);
    r->passlen = (unsigned)n;
    r->flags = soft ? CRYPT_FLAG_FORCE_SOFT : 0;
    crypto_wipe(pw, sizeof(pw));
    long rc = cryptctl(CRYPT_OP_OPEN, r, 0, 0);
    int err = errno;
    crypto_wipe(r->pass, sizeof(r->pass));
    if(rc < 0){
        fprintf(stderr, "cryptsetup: open %s: %s\n", dev,
                err == EACCES ? "parola gresita" : err == EINVAL ? "nu e un volum YKCRYPT valid (sau nume gresit)" :
                err == EBUSY ? "dispozitiv ocupat (deja deschis sau montat)" : strerror(err));
        return 1;
    }
    printf("%s\n", r->name);
    free(r);
    return 0;
}

static int cmd_close(const char *name){
    char nm[CRYPT_NAME_MAX] = "";
    snprintf(nm, sizeof(nm), "%s", name);
    if(cryptctl(CRYPT_OP_CLOSE, nm, 0, 0) < 0){
        fprintf(stderr, "cryptsetup: close %s: %s\n", name, errno == EBUSY ? "volumul e montat sau in folosinta" : strerror(errno));
        return 1;
    }
    return 0;
}

static int cmd_status(const char *name){
    struct crypt_info ci;
    memset(&ci, 0, sizeof(ci));
    snprintf(ci.name, sizeof(ci.name), "%s", name);
    if(cryptctl(CRYPT_OP_INFO, &ci, 0, 0) < 0){
        fprintf(stderr, "cryptsetup: %s: %s\n", name, errno == ENODEV ? "inactiv" : strerror(errno));
        return 1;
    }
    printf("/dev/%s este activ\n  cifru:    %s\n  parinte:  %s\n  sectoare: %lu (de 512 octeti)\n  antet:    %lu sectoare\n  AES-NI:   %s\n",
           ci.name, cipher_name(ci.cipher), ci.dev, (unsigned long)ci.sectors, (unsigned long)ci.offset, ci.hw_aes ? "da" : "nu");
    return 0;
}

static int cmd_dump(const char *dev){
    static uint8_t hdr[YKC_HDR_SIZE];
    struct ykc_info in;
    if(load_hdr(dev, hdr)){
        return 1;
    }
    if(ykc_parse(hdr, &in)){
        fprintf(stderr, "cryptsetup: %s nu are un antet YKCRYPT1 valid\n", dev);
        return 1;
    }
    printf("YKCRYPT1 pe %s\n  cifru:    %s\n  iteratii: %u\n  sector:   %u\n  date:     %lu octeti de la %lu\n  uuid:     ", dev,
           cipher_name(in.cipher), in.iterations, in.sector, (unsigned long)in.payload_bytes, (unsigned long)in.payload_offset);
    for(int i = 0; i < 16; i++){
        printf("%02x", in.uuid[i]);
    }
    printf("\n");
    return 0;
}

static int cmd_passwd(const char *dev, unsigned iter){
    static uint8_t hdr[YKC_HDR_SIZE];
    char oldp[PASS_BUF], newp[PASS_BUF];
    if(load_hdr(dev, hdr)){
        return 1;
    }
    int on = get_pass(pw_opt, key_opt, "Parola veche: ", 0, oldp);
    if(on < 0){
        return 1;
    }
    int nn = get_pass(pw_new, NULL, "Parola noua: ", 1, newp);
    if(nn < 0){
        return 1;
    }
    uint8_t rnd[44];
    if(adb_random(rnd, sizeof(rnd))){
        return 1;
    }
    int rc = ykc_rewrap(hdr, oldp, (size_t)on, newp, (size_t)nn, iter, rnd);
    crypto_wipe(oldp, sizeof(oldp));
    crypto_wipe(newp, sizeof(newp));
    if(rc){
        fprintf(stderr, "cryptsetup: passwd: %s\n", rc == -2 ? "parola veche gresita" : "antet invalid");
        return 1;
    }
    int fd = open(dev, O_WRONLY);
    if(fd < 0 || write_all(fd, hdr, YKC_HDR_SIZE)){
        fprintf(stderr, "cryptsetup: scrierea antetului a esuat\n");
        return 1;
    }
    close(fd);
    printf("cryptsetup: parola schimbata pe %s\n", dev);
    return 0;
}

int main(int argc, char **argv){
    if(argc < 2 || strcmp(argv[1], "--help") == 0){
        usage();
        return argc < 2 ? 1 : 0;
    }
    const char *cmd = argv[1];
    unsigned cipher = YKC_AES_XTS, iter = YKC_ITER_DEFAULT;
    int yes = 0, weak = 0, soft = 0;
    const char *pos[2] = {NULL, NULL};
    int npos = 0;
    for(int i = 2; i < argc; i++){
        const char *a = argv[i];
        if(strcmp(a, "-p") == 0 && i + 1 < argc){
            pw_opt = argv[++i];
        } else if(strcmp(a, "-n") == 0 && i + 1 < argc){
            pw_new = argv[++i];
        } else if(strcmp(a, "-k") == 0 && i + 1 < argc){
            key_opt = argv[++i];
        } else if(strcmp(a, "-i") == 0 && i + 1 < argc){
            iter = (unsigned)strtoul(argv[++i], NULL, 10);
        } else if(strcmp(a, "-c") == 0 && i + 1 < argc){
            const char *c = argv[++i];
            if(strcmp(c, "xts") == 0 || strcmp(c, "aes-xts-plain64") == 0){
                cipher = YKC_AES_XTS;
            } else if(strcmp(c, "chacha20") == 0 || strcmp(c, "chacha20-plain64") == 0){
                cipher = YKC_CHACHA20;
            } else {
                fprintf(stderr, "cryptsetup: cifru necunoscut %s (xts | chacha20)\n", c);
                return 1;
            }
        } else if(strcmp(a, "-y") == 0){
            yes = 1;
        } else if(strcmp(a, "--insecure-rng") == 0){
            weak = 1;
        } else if(strcmp(a, "--soft") == 0){
            soft = 1;
        } else if(a[0] != '-' && npos < 2){
            pos[npos++] = a;
        } else {
            usage();
            return 1;
        }
    }
    if(strcmp(cmd, "format") == 0 && npos == 1){
        return cmd_format(pos[0], cipher, iter, yes, weak);
    }
    if(strcmp(cmd, "open") == 0 && npos >= 1){
        return cmd_open(pos[0], pos[1], soft);
    }
    if(strcmp(cmd, "close") == 0 && npos == 1){
        return cmd_close(pos[0]);
    }
    if(strcmp(cmd, "status") == 0 && npos == 1){
        return cmd_status(pos[0]);
    }
    if(strcmp(cmd, "dump") == 0 && npos == 1){
        return cmd_dump(pos[0]);
    }
    if(strcmp(cmd, "passwd") == 0 && npos == 1){
        return cmd_passwd(pos[0], iter);
    }
    usage();
    return 1;
}

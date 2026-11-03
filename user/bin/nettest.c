/* nettest (T9.T): test de integrare al retelei, pornit de kernel (kernel/tests/test_net.cpp) dupa ce DHCP a dat adresa.
 * uz: nettest <port-http-gazda>
 * Pasi: (1) configuratia DHCP (ifconfig, /run/resolv.conf); (2) ping 10.0.2.2; (3) asculta pe TCP 7 si tipareste
 * "astept host" - tests/net_server.py porneste pe gazda un server HTTP si se conecteaza prin hostfwd la portul 7;
 * (4) descarca /file.sha256 si /file.bin cu /bin/wget si verifica SHA-256; (5) echo TCP: citeste de la gazda pana la EOF
 * si trimite inapoi aceiasi octeti. Iesire 0 = totul bine. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; printf("nettest: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } \
} while(0)

/* ---- SHA-256 ---- */
typedef struct { unsigned h[8]; unsigned char buf[64]; unsigned long long len; unsigned fill; } Sha;

static const unsigned K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(Sha *s, const unsigned char *p){
    unsigned w[64], a, b, c, d, e, f, g, h;
    for(int i = 0; i < 16; i++){
        w[i] = (unsigned)p[4 * i] << 24 | (unsigned)p[4 * i + 1] << 16 | (unsigned)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for(int i = 16; i < 64; i++){
        unsigned s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        unsigned s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for(int i = 0; i < 64; i++){
        unsigned t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
        unsigned t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha *s){
    static const unsigned iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    memcpy(s->h, iv, sizeof(iv));
    s->len = 0;
    s->fill = 0;
}

static void sha_update(Sha *s, const void *data, size_t n){
    const unsigned char *p = data;
    s->len += n;
    while(n){
        unsigned take = 64 - s->fill;
        if(take > n){
            take = (unsigned)n;
        }
        memcpy(s->buf + s->fill, p, take);
        s->fill += take;
        p += take;
        n -= take;
        if(s->fill == 64){
            sha_block(s, s->buf);
            s->fill = 0;
        }
    }
}

static void sha_hex(Sha *s, char out[65]){
    unsigned long long bits = s->len * 8;
    unsigned char pad = 0x80;
    sha_update(s, &pad, 1);
    pad = 0;
    while(s->fill != 56){
        sha_update(s, &pad, 1);
    }
    unsigned char lenb[8];
    for(int i = 0; i < 8; i++){
        lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha_update(s, lenb, 8);
    for(int i = 0; i < 8; i++){
        snprintf(out + 8 * i, 9, "%08x", s->h[i]);
    }
}

/* ---- ajutoare ---- */

static int wait_for(pid_t pid){
    int status = 0;
    if(waitpid(pid, &status, 0) != pid){
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + WTERMSIG(status);
}

/* Ruleaza /bin/<prog> cu argumentele date; stdout+stderr in `output`; intoarce codul de iesire */
static int run(const char *output, const char *prog, const char *a1, const char *a2, const char *a3, const char *a4, const char *a5){
    char path[64];
    snprintf(path, sizeof(path), "/bin/%s", prog);
    char *av[8] = {(char *)prog, (char *)a1, (char *)a2, (char *)a3, (char *)a4, (char *)a5, NULL, NULL};
    pid_t pid = fork();
    if(pid < 0){
        return -1;
    }
    if(pid == 0){
        int in = open("/dev/null", O_RDONLY);
        int o = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if(in < 0 || o < 0){
            _exit(126);
        }
        dup2(in, 0);
        dup2(o, 1);
        dup2(o, 2);
        execve(path, av, environ);
        _exit(127);
    }
    return wait_for(pid);
}

static char text[4096];

static int slurp(const char *path){
    int fd = open(path, O_RDONLY);
    text[0] = 0;
    if(fd < 0){
        return -1;
    }
    int n = (int)read(fd, text, sizeof(text) - 1);
    close(fd);
    if(n < 0){
        n = 0;
    }
    text[n] = 0;
    return n;
}

/* SHA-256 si marimea unui fisier; -1 daca nu se poate citi */
static long file_sha(const char *path, char hex[65]){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return -1;
    }
    static unsigned char buf[8192];
    Sha s;
    sha_init(&s);
    long total = 0;
    int n;
    while((n = (int)read(fd, buf, sizeof(buf))) > 0){
        sha_update(&s, buf, (size_t)n);
        total += n;
    }
    close(fd);
    sha_hex(&s, hex);
    return total;
}

/* wget cu cateva reincercari (serverul de pe gazda poate porni dupa noi) */
static int fetch(const char *url, const char *dest){
    for(int i = 0; i < 40; i++){
        char *av[] = {"wget", "-q", "-T", "20", "-O", (char *)dest, (char *)url, NULL};
        pid_t pid = fork();
        if(pid == 0){
            int o = open("/tmp/wget.out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            dup2(o, 1);
            dup2(o, 2);
            execve("/bin/wget", av, environ);
            _exit(127);
        }
        if(wait_for(pid) == 0){
            return 0;
        }
        usleep(500000);
    }
    return -1;
}

/* Asteapta cel mult ms milisecunde ca fd sa fie gata de citit */
static int ready(int fd, int ms){
    struct pollfd p = {fd, POLLIN, 0};
    return poll(&p, 1, ms) == 1 && (p.revents & POLLIN);
}

int main(int argc, char **argv){
    if(argc != 2){
        fprintf(stderr, "uz: nettest <port-http-gazda>\n");
        return 2;
    }
    const char *http = argv[1];
    char url[128], hex[65];

    /* 1. configuratia primita prin DHCP */
    CHECK(run("/tmp/nt_if", "ifconfig", "eth0", NULL, NULL, NULL, NULL) == 0);
    slurp("/tmp/nt_if");
    CHECK(strstr(text, "10.0.2.15") != NULL);
    slurp("/run/resolv.conf");
    CHECK(strstr(text, "nameserver 10.0.2.3") != NULL);
    printf("nettest: [OK] dhcp (10.0.2.15, DNS 10.0.2.3)\n");
    fflush(stdout);

    /* 2. ping la gateway */
    CHECK(run("/tmp/nt_ping", "ping", "-c", "3", "10.0.2.2", NULL, NULL) == 0);
    slurp("/tmp/nt_ping");
    CHECK(strstr(text, "3 primite") != NULL);
    printf("nettest: [OK] ping 10.0.2.2\n");
    fflush(stdout);

    /* 3. server echo pe portul 7 (gazda ajunge la el prin hostfwd) */
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(7);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    CHECK(ls >= 0 && bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0 && listen(ls, 4) == 0);
    printf("nettest: astept host\n");
    fflush(stdout);

    /* 4. descarcare + SHA-256 */
    chdir("/tmp");
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/file.sha256", http);
    CHECK(fetch(url, "/tmp/file.sha256") == 0);
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/file.bin", http);
    CHECK(fetch(url, "/tmp/file.bin") == 0);
    slurp("/tmp/file.sha256");
    long size = file_sha("/tmp/file.bin", hex);
    CHECK(size == 300000);
    CHECK(strlen(text) >= 64 && strncmp(text, hex, 64) == 0);
    if(failed){
        printf("nettest: sha asteptat %.64s, primit %s, %ld octeti\n", text, hex, size);
    }
    unlink("/tmp/file.bin");
    unlink("/tmp/file.sha256");
    printf("nettest: [OK] wget + sha256 (%ld octeti)\n", size);
    fflush(stdout);

    /* 5. echo TCP: tot ce trimite gazda pana la EOF se trimite inapoi */
    int c = -1;
    if(ready(ls, 90000)){
        c = accept(ls, NULL, NULL);
    }
    CHECK(c >= 0);
    long echoed = 0;
    if(c >= 0){
        static char buf[4096];
        for(;;){
            if(!ready(c, 60000)){
                break;
            }
            int n = (int)read(c, buf, sizeof(buf));
            if(n <= 0){
                break;
            }
            for(int off = 0; off < n;){
                int w = (int)write(c, buf + off, (size_t)(n - off));
                if(w <= 0){
                    break;
                }
                off += w;
            }
            echoed += n;
        }
        close(c);
    }
    close(ls);
    CHECK(echoed == 200000);
    printf("nettest: [OK] echo TCP prin hostfwd (%ld octeti)\n", echoed);

    printf("nettest: %d verificari, %d esuate\n", checks, failed);
    if(!failed){
        printf("[OK] nettest\n");
    }
    fflush(stdout);
    return failed ? 1 : 0;
}

/* Teste T9.4: DHCP/DNS/statistici din userland. Pornit de kernel (kernel/tests/test_dhcp.cpp) dupa ce DHCP a dat adresa;
 * argv[1] = portul UDP al serverului DNS de pe gazda (tests/dns_host.py), la care ajungem prin 10.0.2.2#port.
 * Verifica /etc/resolv.conf (symlink spre /run/resolv.conf scris de kernel), /proc/net/dev, /etc/hosts, gethostbyname si
 * getaddrinfo (A, CNAME + comprimare, NXDOMAIN, server care nu raspunde). Iesire 0 = totul bine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; printf("dnstest: FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); fflush(stdout); } \
} while(0)

static long now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int slurp(const char *path, char *buf, int cap){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return -1;
    }
    int n = 0, r;
    while(n < cap - 1 && (r = (int)read(fd, buf + n, (size_t)(cap - 1 - n))) > 0){
        n += r;
    }
    close(fd);
    buf[n] = 0;
    return n;
}

static int put_resolv(const char *text){
    int fd = open("/run/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd < 0){
        return -1;
    }
    int r = (int)write(fd, text, strlen(text));
    close(fd);
    return r == (int)strlen(text) ? 0 : -1;
}

static const char *ip_str(const struct hostent *h){
    return inet_ntoa(*(struct in_addr *)h->h_addr_list[0]);
}

/* "eth0:" -> contoarele din /proc/net/dev: bytes, packets (rx), apoi tx; 0 daca interfata lipseste */
static int netdev_line(const char *text, const char *name, unsigned long long v[16]){
    size_t nl = strlen(name);
    for(const char *p = text; *p;){
        const char *eol = strchr(p, '\n');
        const char *q = p;
        while(*q == ' '){
            q++;
        }
        if(strncmp(q, name, nl) == 0 && q[nl] == ':'){
            q += nl + 1;
            for(int i = 0; i < 16; i++){
                char *end;
                v[i] = strtoull(q, &end, 10);
                if(end == q){
                    return 0;
                }
                q = end;
            }
            return 1;
        }
        if(!eol){
            break;
        }
        p = eol + 1;
    }
    return 0;
}

static void test_state(void){
    char buf[1024];
    CHECK(slurp("/etc/resolv.conf", buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "nameserver 10.0.2.3\n") != NULL);

    char dev[2048];
    CHECK(slurp("/proc/net/dev", dev, sizeof(dev)) > 0);
    CHECK(strstr(dev, "Receive") != NULL && strstr(dev, "Transmit") != NULL);
    unsigned long long v[16];
    CHECK(netdev_line(dev, "eth0", v));
    CHECK(v[0] > 0 && v[1] > 0);                /* RX bytes / packets: oferta DHCP, ARP, raspunsul ARP-probe */
    CHECK(v[8] > 0 && v[9] > 0);                /* TX bytes / packets: DISCOVER, REQUEST, ARP */
    CHECK(v[1] >= 2 && v[9] >= 2);
    CHECK(v[2] == 0 && v[10] == 0);             /* fara erori */
    unsigned long long rx_before = v[1], tx_before = v[9];
    printf("dnstest: eth0 rx %llu pachete / %llu octeti, tx %llu pachete / %llu octeti\n", v[1], v[0], v[9], v[8]);

    /* contoarele cresc cand trimitem un pachet UDP (spre gazda: ARP deja rezolvat de DHCP sau nu, tot trimitem) */
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(s >= 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    inet_pton(AF_INET, "10.0.2.2", &a.sin_addr);
    CHECK(sendto(s, "x", 1, 0, (struct sockaddr *)&a, sizeof(a)) == 1);
    close(s);
    usleep(300000);
    CHECK(slurp("/proc/net/dev", dev, sizeof(dev)) > 0 && netdev_line(dev, "eth0", v));
    CHECK(v[9] > tx_before);
    (void)rx_before;
}

static void test_hosts_numeric(void){
    struct hostent *h = gethostbyname("127.0.0.1");
    CHECK(h && strcmp(ip_str(h), "127.0.0.1") == 0);
    h = gethostbyname("localhost");
    CHECK(h && strcmp(ip_str(h), "127.0.0.1") == 0);
    h = gethostbyname("host.yokai");            /* din /etc/hosts */
    CHECK(h && strcmp(ip_str(h), "10.0.2.2") == 0);
    h = gethostbyname("HOST.Yokai");            /* fara diferente de litere */
    CHECK(h && strcmp(ip_str(h), "10.0.2.2") == 0);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_NUMERICHOST;
    CHECK(getaddrinfo("example.com", "80", &hints, &res) == EAI_NONAME);   /* AI_NUMERICHOST: fara DNS */
    CHECK(getaddrinfo("1.2.3.4", "80", &hints, &res) == 0);
    if(res){
        freeaddrinfo(res);
    }
}

static void test_dns(int port){
    char conf[64];
    snprintf(conf, sizeof(conf), "nameserver 10.0.2.2#%d\n", port);
    CHECK(put_resolv(conf) == 0);

    struct hostent *h = gethostbyname("yokai.test");
    CHECK(h != NULL);
    if(h){
        CHECK(strcmp(ip_str(h), "10.9.8.7") == 0);
        printf("dnstest: yokai.test -> %s\n", ip_str(h));
    }
    /* raspuns cu CNAME (nume comprimat) urmat de A */
    h = gethostbyname("www.yokai.test");
    CHECK(h != NULL && strcmp(ip_str(h), "10.9.8.8") == 0);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    CHECK(getaddrinfo("yokai.test", "8080", &hints, &res) == 0 && res != NULL);
    if(res){
        struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
        CHECK(sa->sin_port == htons(8080));
        CHECK(strcmp(inet_ntoa(sa->sin_addr), "10.9.8.7") == 0);
        freeaddrinfo(res);
        res = NULL;
    }
    /* NXDOMAIN */
    h_errno = 0;
    CHECK(gethostbyname("nu-exista.yokai.test") == NULL && h_errno == HOST_NOT_FOUND);
    memset(&hints, 0, sizeof(hints));
    CHECK(getaddrinfo("nu-exista.yokai.test", NULL, &hints, &res) == EAI_NONAME);
    /* nume invalide nu pleaca pe retea */
    CHECK(gethostbyname("a b") == NULL);
    CHECK(gethostbyname("..") == NULL);
    CHECK(gethostbyname("") == NULL);

    /* server care nu raspunde: EAI_AGAIN dupa 2 incercari */
    CHECK(put_resolv("nameserver 10.0.2.2#1\n") == 0);
    long t0 = now_ms();
    h_errno = 0;
    memset(&hints, 0, sizeof(hints));
    CHECK(getaddrinfo("yokai.test", NULL, &hints, &res) == EAI_AGAIN);
    long dt = now_ms() - t0;
    printf("dnstest: server mort: EAI_AGAIN dupa %ld ms\n", dt);
    CHECK(dt >= 1500 && dt < 20000);

    /* fara servere: la fel */
    CHECK(put_resolv("# gol\n") == 0);
    CHECK(gethostbyname("yokai.test") == NULL);
}

/* DNS-ul real al slirp (10.0.2.3 din DHCP): doar informativ, pe gazde fara retea nu raspunde */
static void test_slirp(void){
    CHECK(put_resolv("nameserver 10.0.2.3\n") == 0);
    long t0 = now_ms();
    struct hostent *h = gethostbyname("example.com");
    printf("dnstest: slirp dns example.com -> %s (%ld ms, informativ)\n", h ? ip_str(h) : "nerezolvat", now_ms() - t0);
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    int port = argc > 1 ? atoi(argv[1]) : 0;
    char saved[256];
    int sn = slurp("/run/resolv.conf", saved, sizeof(saved));
    test_state();
    test_hosts_numeric();
    if(port > 0){
        test_dns(port);
    } else {
        printf("dnstest: fara portul DNS al gazdei: sar peste testele de DNS\n");
    }
    test_slirp();
    if(sn > 0){
        saved[sn] = 0;
        put_resolv(saved);
    }
    printf("dnstest: %d verificari, %d esuate\n", checks, failed);
    if(failed){
        return 1;
    }
    printf("[OK] dnstest\n");
    return 0;
}

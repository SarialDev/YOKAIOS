/* ping: ICMP echo printr-un socket brut (SOCK_RAW, IPPROTO_ICMP; cere CAP_NET), cu statistici RTT (T9.5).
 * uz: ping [-c numar] [-s octeti] [-W secunde] [-i secunde] gazda      (-c implicit 4; -c 0 = pana la Ctrl+C)
 * iesire: 0 daca a venit cel putin un raspuns, 1 altfel, 2 la eroare de utilizare */
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include "netutil.h"

static volatile int stop;

static void on_int(int sig){
    (void)sig;
    stop = 1;
}

static unsigned short cksum(const void *data, int len){
    const unsigned char *p = data;
    unsigned long sum = 0;
    while(len > 1){
        sum += (unsigned long)(p[0] << 8 | p[1]);
        p += 2;
        len -= 2;
    }
    if(len){
        sum += (unsigned long)(p[0] << 8);
    }
    while(sum >> 16){
        sum = (sum & 0xffff) + (sum >> 16);
    }
    return htons((unsigned short)~sum);       /* octetii in ordinea retelei */
}

/* "1.234" din microsecunde */
static void fmt_ms(char *buf, size_t n, long long us){
    snprintf(buf, n, "%lld.%03lld", us / 1000, us % 1000);
}

static unsigned long long isqrt(unsigned long long v){
    unsigned long long r = 0, b = 1ULL << 62;
    while(b > v){
        b >>= 2;
    }
    while(b){
        if(v >= r + b){
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

struct icmp_hdr {
    unsigned char type, code;
    unsigned short sum;
    unsigned short id, seq;
};

int main(int argc, char **argv){
    long count = 4;
    unsigned long size = 56, wait_s = 5, interval_s = 1, v;
    const char *host = NULL;
    for(int i = 1; i < argc; i++){
        const char *a = argv[i];
        if(!strcmp(a, "--help")){
            puts("uz: ping [-c numar] [-s octeti] [-W secunde] [-i secunde] gazda");
            return 0;
        }
        if(a[0] == '-' && a[1] && !a[2] && strchr("csWi", a[1])){
            if(i + 1 >= argc || !nu_parse_uint(argv[i + 1], 1000000, &v)){
                fprintf(stderr, "ping: valoare gresita pentru %s\n", a);
                return 2;
            }
            i++;
            switch(a[1]){
            case 'c': count = (long)v; break;
            case 's': size = v; break;
            case 'W': wait_s = v; break;
            default: interval_s = v; break;
            }
        } else if(a[0] == '-' && a[1]){
            fprintf(stderr, "ping: optiune necunoscuta %s\n", a);
            return 2;
        } else if(!host){
            host = a;
        } else {
            fprintf(stderr, "uz: ping [-c numar] [-s octeti] [-W secunde] [-i secunde] gazda\n");
            return 2;
        }
    }
    if(!host || size > 1400 || wait_s < 1){
        fprintf(stderr, "uz: ping [-c numar] [-s octeti] [-W secunde] [-i secunde] gazda\n");
        return 2;
    }
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    if(nu_resolve(host, &to.sin_addr) < 0){
        fprintf(stderr, "ping: %s: nume necunoscut\n", host);
        return 2;
    }
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if(fd < 0){
        perror("ping: socket brut (necesita CAP_NET si retea pornita)");
        return 2;
    }
    signal(SIGINT, on_int);

    unsigned short id = (unsigned short)(getpid() & 0xffff);
    unsigned char pkt[8 + 8 + 1400], rbuf[2048];
    int plen = 8 + 8 + (int)size;       /* antet ICMP + marca de timp (8) + date */
    char ipstr[16];
    snprintf(ipstr, sizeof(ipstr), "%s", inet_ntoa(to.sin_addr));
    printf("PING %s (%s): %lu octeti de date\n", host, ipstr, size);

    long sent = 0, recvd = 0, errors = 0;
    long long rtt_min = 0, rtt_max = 0, rtt_sum = 0;
    unsigned long long rtt_sq = 0;
    long long t_start = nu_now_us();

    while(!stop && (count == 0 || sent < count)){
        struct icmp_hdr *h = (struct icmp_hdr *)pkt;
        memset(pkt, 0, sizeof(pkt));
        h->type = 8;
        h->id = htons(id);
        h->seq = htons((unsigned short)(sent + 1));
        long long t_send = nu_now_us();
        memcpy(pkt + 8, &t_send, 8);
        for(int i = 16; i < plen; i++){
            pkt[i] = (unsigned char)(i - 16);
        }
        h->sum = cksum(pkt, plen);
        if(sendto(fd, pkt, (size_t)plen, 0, (struct sockaddr *)&to, sizeof(to)) < 0){
            printf("ping: sendto: %s\n", strerror(errno));
            errors++;
        }
        sent++;
        long long deadline = t_send + (long long)wait_s * 1000000LL;
        int got = 0;
        while(!stop && !got){
            long long now = nu_now_us();
            if(now >= deadline){
                break;
            }
            struct pollfd p = {fd, POLLIN, 0};
            int ms = (int)((deadline - now + 999) / 1000);
            int pr = poll(&p, 1, ms);
            if(pr <= 0){
                continue;
            }
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(fd, rbuf, sizeof(rbuf), 0, (struct sockaddr *)&from, &fl);
            if(n < 8){
                continue;
            }
            int off = 0, ttl = 0;
            if((rbuf[0] >> 4) == 4){            /* pachetul vine cu antet IP */
                off = (rbuf[0] & 15) * 4;
                ttl = rbuf[8];
            }
            if(n < off + 8){
                continue;
            }
            struct icmp_hdr *r = (struct icmp_hdr *)(rbuf + off);
            if(r->type == 0 && ntohs(r->id) == id && ntohs(r->seq) >= 1 && ntohs(r->seq) <= (unsigned short)sent){
                long long t0 = 0;
                if(n >= off + 16){
                    memcpy(&t0, rbuf + off + 8, 8);
                }
                long long rtt = nu_now_us() - (t0 ? t0 : t_send);
                char ms_s[24];
                fmt_ms(ms_s, sizeof(ms_s), rtt);
                printf("%ld octeti de la %s: icmp_seq=%u ttl=%d timp=%s ms\n", (long)(n - off - 8), inet_ntoa(from.sin_addr),
                       (unsigned)ntohs(r->seq), ttl, ms_s);
                if(ntohs(r->seq) == (unsigned short)sent){
                    got = 1;
                }
                if(!recvd || rtt < rtt_min){
                    rtt_min = rtt;
                }
                if(rtt > rtt_max){
                    rtt_max = rtt;
                }
                rtt_sum += rtt;
                rtt_sq += (unsigned long long)rtt * (unsigned long long)rtt;
                recvd++;
            } else if(r->type == 3 && n >= off + 8 + 20 + 8){
                /* destinatie inaccesibila pentru un echo al nostru (antetul IP original + 8 octeti) */
                struct icmp_hdr *o = (struct icmp_hdr *)(rbuf + off + 8 + 20);
                if(o->type == 8 && ntohs(o->id) == id){
                    printf("De la %s: icmp_seq=%u destinatie inaccesibila (cod %d)\n", inet_ntoa(from.sin_addr),
                           (unsigned)ntohs(o->seq), r->code);
                    errors++;
                    got = 1;
                }
            }
        }
        if(stop || (count != 0 && sent >= count)){
            break;
        }
        /* pana la urmatorul pachet */
        long long next = t_send + (long long)interval_s * 1000000LL;
        while(!stop){
            long long now = nu_now_us();
            if(now >= next){
                break;
            }
            usleep((unsigned)((next - now) > 100000 ? 100000 : (next - now)));
        }
    }
    long long total_ms = (nu_now_us() - t_start) / 1000;
    printf("\n--- %s statistici ping ---\n", host);
    long loss = sent ? (long)(100 * (sent - recvd) / sent) : 0;
    printf("%ld pachete trimise, %ld primite, %ld%% pierdute, timp %lld ms\n", sent, recvd, loss, total_ms);
    if(recvd){
        long long avg = rtt_sum / recvd;
        unsigned long long mean_sq = rtt_sq / (unsigned long long)recvd;
        unsigned long long var = mean_sq > (unsigned long long)(avg * avg) ? mean_sq - (unsigned long long)(avg * avg) : 0;
        char a[24], b[24], c[24], d[24];
        fmt_ms(a, sizeof(a), rtt_min);
        fmt_ms(b, sizeof(b), avg);
        fmt_ms(c, sizeof(c), rtt_max);
        fmt_ms(d, sizeof(d), (long long)isqrt(var));
        printf("rtt min/avg/max/mdev = %s/%s/%s/%s ms\n", a, b, c, d);
    }
    close(fd);
    return recvd > 0 ? 0 : 1;
}

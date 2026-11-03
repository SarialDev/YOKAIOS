/* Rezolvarea de nume (T9.4): /etc/hosts, apoi DNS peste UDP (A, IPv4) spre serverele din /etc/resolv.conf
 * (scris de kernel din oferta DHCP: /etc/resolv.conf -> /run/resolv.conf). Extensie: "nameserver a.b.c.d#port". */
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>

#define MAXNS 3
#define FILE_MAX 2048

struct ns {
    uint32_t ip;
    uint16_t port;
};

static int read_file(const char *path, char *buf, size_t cap){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return -1;
    }
    size_t n = 0;
    for(;;){
        ssize_t r = read(fd, buf + n, cap - 1 - n);
        if(r <= 0){
            break;
        }
        n += (size_t)r;
        if(n >= cap - 1){
            break;
        }
    }
    close(fd);
    buf[n] = 0;
    return (int)n;
}

/* Urmatoarea linie din text (in-place, terminata cu 0); intoarce NULL la sfarsit. '#' si ';' incep comentarii (nu cel din ip#port) */
static char *next_line(char **pp){
    char *p = *pp;
    if(!*p){
        return NULL;
    }
    char *line = p;
    while(*p && *p != '\n'){
        p++;
    }
    if(*p){
        *p++ = 0;
    }
    *pp = p;
    return line;
}

static int is_ws(char c){
    return c == ' ' || c == '\t' || c == '\r';
}

static char *skip_ws(char *s){
    while(is_ws(*s)){
        s++;
    }
    return s;
}

static char *word_end(char *s){
    while(*s && !is_ws(*s)){
        s++;
    }
    return s;
}

static int parse_ip(const char *s, uint32_t *ip){
    struct in_addr a;
    if(inet_pton(AF_INET, s, &a) != 1){
        return 0;
    }
    *ip = a.s_addr;
    return 1;
}

static int load_ns(struct ns *v){
    char buf[FILE_MAX];
    int n = 0;
    if(read_file("/etc/resolv.conf", buf, sizeof(buf)) < 0){
        return 0;
    }
    char *p = buf, *line;
    while((line = next_line(&p)) && n < MAXNS){
        line = skip_ws(line);
        if(strncmp(line, "nameserver", 10) != 0 || !is_ws(line[10])){
            continue;
        }
        char *w = skip_ws(line + 10);
        char *e = word_end(w);
        *e = 0;
        uint16_t port = 53;
        char *hash = strchr(w, '#');
        if(hash){
            *hash = 0;
            long pv = strtol(hash + 1, NULL, 10);
            if(pv > 0 && pv < 65536){
                port = (uint16_t)pv;
            }
        }
        uint32_t ip;
        if(parse_ip(w, &ip) && ip){
            v[n].ip = ip;
            v[n].port = port;
            n++;
        }
    }
    return n;
}

static int ieq(const char *a, const char *b){
    for(;; a++, b++){
        char x = *a, y = *b;
        if(x >= 'A' && x <= 'Z'){
            x = (char)(x + 32);
        }
        if(y >= 'A' && y <= 'Z'){
            y = (char)(y + 32);
        }
        if(x != y){
            return 0;
        }
        if(!x){
            return 1;
        }
    }
}

static int hosts_lookup(const char *name, uint32_t *ip){
    char buf[FILE_MAX];
    if(read_file("/etc/hosts", buf, sizeof(buf)) < 0){
        return 0;
    }
    char *p = buf, *line;
    while((line = next_line(&p))){
        char *hash = strchr(line, '#');
        if(hash){
            *hash = 0;
        }
        char *w = skip_ws(line);
        char *e = word_end(w);
        if(w == e){
            continue;
        }
        int more = *e != 0;
        *e = 0;
        uint32_t a;
        if(!more || !parse_ip(w, &a)){
            continue;
        }
        w = e + 1;
        for(;;){
            w = skip_ws(w);
            e = word_end(w);
            if(w == e){
                break;
            }
            int last = *e == 0;
            *e = 0;
            if(ieq(w, name)){
                *ip = a;
                return 1;
            }
            if(last){
                break;
            }
            w = e + 1;
        }
    }
    return 0;
}

/* Nume DNS valid: etichete de 1..63, total <= 253, caractere alfanumerice, '-' , '_' ; un punct final se accepta */
static int build_query(const char *name, uint16_t id, uint8_t *q, size_t cap, size_t *len){
    size_t nl = strlen(name);
    if(nl == 0 || nl > 253 || cap < 12 + nl + 2 + 4){
        return 0;
    }
    memset(q, 0, 12);
    q[0] = (uint8_t)(id >> 8);
    q[1] = (uint8_t)id;
    q[2] = 0x01;                            /* RD */
    q[5] = 1;                               /* QDCOUNT */
    size_t o = 12;
    const char *s = name;
    while(*s){
        const char *dot = strchr(s, '.');
        size_t l = dot ? (size_t)(dot - s) : strlen(s);
        if(l == 0 || l > 63){
            if(!dot && l == 0){
                break;                      /* punct final */
            }
            return 0;
        }
        q[o++] = (uint8_t)l;
        for(size_t i = 0; i < l; i++){
            char c = s[i];
            int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            if(!ok){
                return 0;
            }
            q[o++] = (uint8_t)c;
        }
        s += l;
        if(*s == '.'){
            s++;
        }
    }
    q[o++] = 0;
    q[o++] = 0;
    q[o++] = 1;                             /* QTYPE A */
    q[o++] = 0;
    q[o++] = 1;                             /* QCLASS IN */
    *len = o;
    return 1;
}

/* Sare peste un nume (etichete sau pointer de comprimare); intoarce noua pozitie sau 0 la eroare */
static size_t skip_name(const uint8_t *m, size_t len, size_t o){
    for(;;){
        if(o >= len){
            return 0;
        }
        uint8_t c = m[o];
        if(c == 0){
            return o + 1;
        }
        if((c & 0xC0) == 0xC0){
            return o + 2 <= len ? o + 2 : 0;
        }
        if(c & 0xC0){
            return 0;
        }
        o += 1 + (size_t)c;
    }
}

/* 0 = A gasit, 1 = NXDOMAIN / fara adresa A, -1 = raspuns invalid / eroare de server (se incearca urmatorul server) */
static int parse_response(const uint8_t *m, size_t len, uint16_t id, uint32_t *ip){
    if(len < 12 || (uint16_t)(m[0] << 8 | m[1]) != id || !(m[2] & 0x80)){
        return -1;
    }
    int rcode = m[3] & 15;
    if(rcode == 3){
        return 1;
    }
    if(rcode != 0){
        return -1;
    }
    unsigned qd = (unsigned)(m[4] << 8 | m[5]), an = (unsigned)(m[6] << 8 | m[7]);
    size_t o = 12;
    for(unsigned i = 0; i < qd; i++){
        o = skip_name(m, len, o);
        if(!o || o + 4 > len){
            return -1;
        }
        o += 4;
    }
    for(unsigned i = 0; i < an; i++){
        o = skip_name(m, len, o);
        if(!o || o + 10 > len){
            return -1;
        }
        unsigned type = (unsigned)(m[o] << 8 | m[o + 1]), cls = (unsigned)(m[o + 2] << 8 | m[o + 3]);
        unsigned rdlen = (unsigned)(m[o + 8] << 8 | m[o + 9]);
        o += 10;
        if(o + rdlen > len){
            return -1;
        }
        if(type == 1 && cls == 1 && rdlen == 4){
            memcpy(ip, m + o, 4);
            return 0;
        }
        o += rdlen;                         /* CNAME & co: raspunsul poarta de obicei si A-ul final */
    }
    return 1;
}

/* 0 = gasit; 1 = nu exista; -1 = fara raspuns (fara server / timeout) */
static int dns_query(const char *name, uint32_t *ip){
    struct ns servers[MAXNS];
    int nns = load_ns(servers);
    if(nns == 0){
        return -1;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint16_t id = (uint16_t)(ts.tv_nsec ^ (ts.tv_nsec >> 16) ^ (unsigned long)getpid() * 2654435761u);
    uint8_t q[300];
    size_t qlen;
    if(!build_query(name, id, q, sizeof(q), &qlen)){
        return 1;
    }
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if(fd < 0){
        return -1;
    }
    int result = -1;
    for(int round = 0; round < 2 && result < 0; round++){
        for(int s = 0; s < nns && result < 0; s++){
            struct sockaddr_in sa;
            memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = htons(servers[s].port);
            sa.sin_addr.s_addr = servers[s].ip;
            if(sendto(fd, q, qlen, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0){
                continue;
            }
            int waited = 0;
            while(waited < 2000 && result < 0){
                struct pollfd p = {fd, POLLIN, 0};
                int pr = poll(&p, 1, 2000 - waited);
                if(pr < 0 && errno == EINTR){
                    break;
                }
                if(pr <= 0){
                    break;
                }
                uint8_t r[512];
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                ssize_t n = recvfrom(fd, r, sizeof(r), 0, (struct sockaddr *)&from, &fl);
                waited += 100;                  /* pachetele straine nu prelungesc asteptarea la nesfarsit */
                if(n < 0 || from.sin_addr.s_addr != servers[s].ip){
                    continue;               /* pachet strain */
                }
                int pa = parse_response(r, (size_t)n, id, ip);
                if(pa == 0 || pa == 1){
                    result = pa;
                }
                break;
            }
        }
    }
    close(fd);
    return result;
}

/* Numele -> adresa IPv4 (ordinea retelei): 0 = gasit, 1 = nu exista, -1 = serviciul DNS nu raspunde */
int __resolve_host(const char *name, uint32_t *ip){
    if(!name || !*name){
        return 1;
    }
    if(hosts_lookup(name, ip)){
        return 0;
    }
    return dns_query(name, ip);
}

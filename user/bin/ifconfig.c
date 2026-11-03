/* ifconfig: arata interfetele de retea si seteaza adresa statica (T9.5), prin ioctl SIOC* pe un socket UDP.
 * uz: ifconfig [-a] [interfata]
 *     ifconfig interfata adresa[/prefix] [netmask masca] [gw gateway]      (cere CAP_NET; opreste DHCP)
 * Interfetele se iau din /proc/net/dev; fara -a se arata doar cele cu adresa (UP) */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/route.h>
#include "netutil.h"

static int sock = -1;

static void sin_set(struct sockaddr *sa, struct in_addr a){
    struct sockaddr_in *s = (struct sockaddr_in *)sa;
    memset(sa, 0, sizeof(*sa));
    s->sin_family = AF_INET;
    s->sin_addr = a;
}

static struct in_addr sin_get(const struct sockaddr *sa){
    return ((const struct sockaddr_in *)sa)->sin_addr;
}

static int query(const char *name, unsigned long cmd, struct ifreq *r){
    memset(r, 0, sizeof(*r));
    snprintf(r->ifr_name, IFNAMSIZ, "%s", name);
    return ioctl(sock, cmd, r);
}

/* Contoare din linia "nume:" a /proc/net/dev: v[0..15] */
static int dev_stats(const char *name, unsigned long long v[16]){
    int fd = open("/proc/net/dev", O_RDONLY);
    if(fd < 0){
        return -1;
    }
    char buf[2048];
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if(n <= 0){
        return -1;
    }
    buf[n] = 0;
    size_t nl = strlen(name);
    for(char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")){
        while(*l == ' '){
            l++;
        }
        if(!strncmp(l, name, nl) && l[nl] == ':'){
            char *p = l + nl + 1;
            for(int i = 0; i < 16; i++){
                v[i] = strtoull(p, &p, 10);
            }
            return 0;
        }
    }
    return -1;
}

static void show(const char *name, int all){
    struct ifreq r;
    if(query(name, SIOCGIFFLAGS, &r) < 0){
        fprintf(stderr, "ifconfig: %s: interfata necunoscuta\n", name);
        return;
    }
    short fl = r.ifr_flags;
    if(!all && !(fl & IFF_UP)){
        return;
    }
    char fs[64] = "";
    static const struct { short bit; const char *name; } names[] = {
        {IFF_UP, "UP"}, {IFF_BROADCAST, "BROADCAST"}, {IFF_RUNNING, "RUNNING"}, {IFF_MULTICAST, "MULTICAST"},
    };
    for(unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++){
        if(fl & names[i].bit){
            if(fs[0]){
                strcat(fs, ",");
            }
            strcat(fs, names[i].name);
        }
    }
    int mtu = 0;
    if(query(name, SIOCGIFMTU, &r) == 0){
        mtu = r.ifr_mtu;
    }
    printf("%s: flags=%d<%s>  mtu %d\n", name, (int)fl, fs, mtu);
    if(query(name, SIOCGIFADDR, &r) == 0){
        struct in_addr a = sin_get(&r.ifr_addr);
        printf("        inet %s", inet_ntoa(a));
        if(query(name, SIOCGIFNETMASK, &r) == 0){
            printf("  netmask %s", inet_ntoa(sin_get(&r.ifr_netmask)));
        }
        if(query(name, SIOCGIFBRDADDR, &r) == 0){
            printf("  broadcast %s", inet_ntoa(sin_get(&r.ifr_broadaddr)));
        }
        printf("\n");
    }
    if(query(name, SIOCGIFHWADDR, &r) == 0){
        unsigned char *m = (unsigned char *)r.ifr_hwaddr.sa_data;
        printf("        ether %02x:%02x:%02x:%02x:%02x:%02x\n", m[0], m[1], m[2], m[3], m[4], m[5]);
    }
    unsigned long long v[16];
    if(dev_stats(name, v) == 0){
        printf("        RX packets %llu  bytes %llu  errors %llu  dropped %llu\n", v[1], v[0], v[2], v[3]);
        printf("        TX packets %llu  bytes %llu  errors %llu  dropped %llu\n", v[9], v[8], v[10], v[11]);
    }
    printf("\n");
}

/* Prefix /n -> masca */
static int prefix_mask(const char *s, struct in_addr *m){
    unsigned long n;
    if(!nu_parse_uint(s, 32, &n)){
        return -1;
    }
    unsigned int host = n ? (unsigned int)(0xffffffffu << (32 - n)) : 0;
    m->s_addr = htonl(host);
    return 0;
}

static int set_addr(const char *name, char *addr, const char *mask, const char *gw){
    struct ifreq r;
    struct in_addr a, m, g;
    char *slash = strchr(addr, '/');
    int have_mask = 0;
    if(slash){
        *slash = 0;
        if(prefix_mask(slash + 1, &m) < 0){
            fprintf(stderr, "ifconfig: prefix gresit: %s\n", slash + 1);
            return 1;
        }
        have_mask = 1;
    }
    if(!inet_aton(addr, &a)){
        fprintf(stderr, "ifconfig: adresa gresita: %s\n", addr);
        return 1;
    }
    if(mask){
        if(!inet_aton(mask, &m)){
            fprintf(stderr, "ifconfig: masca gresita: %s\n", mask);
            return 1;
        }
        have_mask = 1;
    }
    if(gw && !inet_aton(gw, &g)){
        fprintf(stderr, "ifconfig: gateway gresit: %s\n", gw);
        return 1;
    }
    if(query(name, SIOCGIFFLAGS, &r) < 0){
        fprintf(stderr, "ifconfig: %s: interfata necunoscuta\n", name);
        return 1;
    }
    /* masca intai, ca adresa noua sa nu aiba momentan o masca veche nepotrivita */
    if(have_mask){
        memset(&r, 0, sizeof(r));
        snprintf(r.ifr_name, IFNAMSIZ, "%s", name);
        sin_set(&r.ifr_netmask, m);
        if(ioctl(sock, SIOCSIFNETMASK, &r) < 0){
            fprintf(stderr, "ifconfig: SIOCSIFNETMASK: %s\n", strerror(errno));
            return 1;
        }
    }
    memset(&r, 0, sizeof(r));
    snprintf(r.ifr_name, IFNAMSIZ, "%s", name);
    sin_set(&r.ifr_addr, a);
    if(ioctl(sock, SIOCSIFADDR, &r) < 0){
        fprintf(stderr, "ifconfig: SIOCSIFADDR: %s\n", strerror(errno));
        return 1;
    }
    if(gw){
        struct rtentry rt;
        memset(&rt, 0, sizeof(rt));
        sin_set(&rt.rt_dst, (struct in_addr){0});
        sin_set(&rt.rt_genmask, (struct in_addr){0});
        sin_set(&rt.rt_gateway, g);
        rt.rt_flags = RTF_UP | RTF_GATEWAY;
        if(ioctl(sock, g.s_addr ? SIOCADDRT : SIOCDELRT, &rt) < 0){
            fprintf(stderr, "ifconfig: ruta implicita: %s\n", strerror(errno));
            return 1;
        }
    }
    return 0;
}

int main(int argc, char **argv){
    int all = 0;
    const char *name = NULL;
    const char *pos[8];
    int npos = 0;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--help")){
            puts("uz: ifconfig [-a] [interfata] | ifconfig interfata adresa[/prefix] [netmask masca] [gw gateway]");
            return 0;
        }
        if(!strcmp(argv[i], "-a")){
            all = 1;
        } else if(argv[i][0] == '-' && argv[i][1]){
            fprintf(stderr, "ifconfig: optiune necunoscuta %s\n", argv[i]);
            return 2;
        } else if(npos < 8){
            pos[npos++] = argv[i];
        }
    }
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if(sock < 0){
        perror("ifconfig: socket");
        return 1;
    }
    if(npos == 0){
        /* toate interfetele din /proc/net/dev */
        int fd = open("/proc/net/dev", O_RDONLY);
        char buf[2048];
        int n = fd >= 0 ? (int)read(fd, buf, sizeof(buf) - 1) : -1;
        if(fd >= 0){
            close(fd);
        }
        if(n <= 0){
            fprintf(stderr, "ifconfig: nu pot citi /proc/net/dev\n");
            return 1;
        }
        buf[n] = 0;
        char names[8][IFNAMSIZ];
        int nn = 0;
        for(char *l = strtok(buf, "\n"); l && nn < 8; l = strtok(NULL, "\n")){
            char *c = strchr(l, ':');
            if(!c || strchr(l, '|')){
                continue;
            }
            *c = 0;
            while(*l == ' '){
                l++;
            }
            snprintf(names[nn++], IFNAMSIZ, "%s", l);
        }
        for(int i = 0; i < nn; i++){
            show(names[i], all);       /* show() foloseste si el strtok: de aceea numele sunt copiate inainte */
        }
        return 0;
    }
    name = pos[0];
    if(npos == 1){
        struct ifreq r;
        if(query(name, SIOCGIFFLAGS, &r) < 0){
            fprintf(stderr, "ifconfig: %s: interfata necunoscuta\n", name);
            return 1;
        }
        show(name, 1);
        return 0;
    }
    if(!strcmp(pos[1], "up") && npos == 2){
        show(name, 1);                  /* interfata e mereu sus */
        return 0;
    }
    char addr[64];
    snprintf(addr, sizeof(addr), "%s", pos[1]);
    const char *mask = NULL, *gw = NULL;
    for(int i = 2; i < npos; i++){
        if(!strcmp(pos[i], "netmask") && i + 1 < npos){
            mask = pos[++i];
        } else if(!strcmp(pos[i], "gw") && i + 1 < npos){
            gw = pos[++i];
        } else if(!strcmp(pos[i], "up")){
            continue;
        } else {
            fprintf(stderr, "ifconfig: argument necunoscut: %s\n", pos[i]);
            return 2;
        }
    }
    return set_addr(name, addr, mask, gw);
}

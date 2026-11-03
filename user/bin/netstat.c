/* netstat: conexiuni si rute din /proc/net/{tcp,udp,route} (T9.5). Adresele sunt mereu numerice.
 * uz: netstat [-a] [-l] [-n] [-t] [-u] [-r]
 *   fara optiuni: conexiuni stabilite; -a: si cele care asculta; -l: doar cele care asculta; -t / -u: doar TCP / UDP;
 *   -r: tabela de rute; -n: acceptat (adresele sunt oricum numerice) */
#include <unistd.h>
#include <fcntl.h>
#include "netutil.h"

static const char *tcp_state(int st){
    static const char *names[] = {"?", "ESTABLISHED", "SYN_SENT", "SYN_RECV", "FIN_WAIT1", "FIN_WAIT2", "TIME_WAIT",
                                  "CLOSE", "CLOSE_WAIT", "LAST_ACK", "LISTEN", "CLOSING"};
    return st >= 1 && st <= 11 ? names[st] : "?";
}

/* "0F02000A" -> "10.0.2.15" (u32 little-endian, ca in /proc/net/tcp) */
static void ip_str(unsigned long v, char *out){
    sprintf(out, "%lu.%lu.%lu.%lu", v & 255, (v >> 8) & 255, (v >> 16) & 255, (v >> 24) & 255);
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

static int show_routes(void){
    static char buf[4096];
    if(slurp("/proc/net/route", buf, sizeof(buf)) < 0){
        fprintf(stderr, "netstat: nu pot citi /proc/net/route\n");
        return 1;
    }
    printf("Tabela de rutare IP\n");
    printf("%-15s %-15s %-15s %-5s %6s %s\n", "Destinatie", "Gateway", "Masca", "Flags", "Metric", "Iface");
    int first = 1;
    for(char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")){
        if(first){
            first = 0;
            continue;
        }
        char iface[32];
        unsigned long dst, gw, flags, mask;
        char *p = l;
        size_t k = 0;
        while(*p && *p != '\t' && k < sizeof(iface) - 1){
            iface[k++] = *p++;
        }
        iface[k] = 0;
        dst = strtoul(p, &p, 16);
        gw = strtoul(p, &p, 16);
        flags = strtoul(p, &p, 16);
        strtoul(p, &p, 10);                 /* RefCnt */
        strtoul(p, &p, 10);                 /* Use */
        unsigned long metric = strtoul(p, &p, 10);
        mask = strtoul(p, &p, 16);
        char d[20], g[20], m[20], f[8];
        ip_str(dst, d);
        ip_str(gw, g);
        ip_str(mask, m);
        f[0] = 0;
        if(flags & 1){
            strcat(f, "U");
        }
        if(flags & 2){
            strcat(f, "G");
        }
        printf("%-15s %-15s %-15s %-5s %6lu %s\n", d, g, m, f, metric, iface);
    }
    return 0;
}

static void show_table(const char *proto, const char *path, int all, int listen_only){
    static char buf[8192];
    if(slurp(path, buf, sizeof(buf)) < 0){
        return;
    }
    int udp = proto[0] == 'u';
    int first = 1;
    for(char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")){
        if(first){
            first = 0;
            continue;
        }
        char *p = strchr(l, ':');
        if(!p){
            continue;
        }
        p++;
        unsigned long la = strtoul(p, &p, 16);
        unsigned long lp = strtoul(p + 1, &p, 16);
        unsigned long ra = strtoul(p, &p, 16);
        unsigned long rp = strtoul(p + 1, &p, 16);
        int st = (int)strtoul(p, &p, 16);
        unsigned long tx = strtoul(p, &p, 16);
        unsigned long rx = strtoul(p + 1, &p, 16);
        int listening = udp ? st == 7 : st == 10;
        if(listen_only ? !listening : (!all && listening)){
            continue;
        }
        char a[24], b[24], local[32], remote[32];
        ip_str(la, a);
        ip_str(ra, b);
        snprintf(local, sizeof(local), "%s:%lu", a, lp);
        if(udp && st == 7){
            snprintf(remote, sizeof(remote), "*:*");
        } else if(listening){
            snprintf(remote, sizeof(remote), "0.0.0.0:*");
        } else {
            snprintf(remote, sizeof(remote), "%s:%lu", b, rp);
        }
        printf("%-5s %6lu %6lu %-23s %-23s %s\n", proto, rx, tx, local, remote, udp ? "" : tcp_state(st));
    }
}

int main(int argc, char **argv){
    int all = 0, listen_only = 0, want_t = 0, want_u = 0, routes = 0;
    for(int i = 1; i < argc; i++){
        const char *a = argv[i];
        if(!strcmp(a, "--help")){
            puts("uz: netstat [-a] [-l] [-n] [-t] [-u] [-r]");
            return 0;
        }
        if(a[0] != '-' || !a[1]){
            fprintf(stderr, "netstat: argument necunoscut: %s\n", a);
            return 2;
        }
        for(const char *c = a + 1; *c; c++){
            switch(*c){
            case 'a': all = 1; break;
            case 'l': listen_only = 1; break;
            case 'n': break;
            case 't': want_t = 1; break;
            case 'u': want_u = 1; break;
            case 'r': routes = 1; break;
            default:
                fprintf(stderr, "netstat: optiune necunoscuta -%c\n", *c);
                return 2;
            }
        }
    }
    if(routes){
        return show_routes();
    }
    if(!want_t && !want_u){
        want_t = want_u = 1;
    }
    printf("Conexiuni Internet active (%s)\n", listen_only ? "doar servere" : all ? "servere si stabilite" : "fara servere");
    printf("%-5s %6s %6s %-23s %-23s %s\n", "Proto", "Recv-Q", "Send-Q", "Adresa locala", "Adresa straina", "Stare");
    if(want_t){
        show_table("tcp", "/proc/net/tcp", all, listen_only);
    }
    if(want_u){
        show_table("udp", "/proc/net/udp", all, listen_only);
    }
    return 0;
}

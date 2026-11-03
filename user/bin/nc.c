/* nc: client / server TCP (si UDP) simplu (T9.5). Copiaza stdin spre conexiune si conexiunea spre stdout.
 * uz: nc [-u] [-z] [-w secunde] gazda port          client
 *     nc -l [-u] [-p port | port] [-w secunde]      server: asteapta o singura conexiune (UDP: primul pachet)
 * Cand stdin se termina trimite FIN (TCP) si mai citeste pana inchide partenerul; la UDP iese. -z: doar verifica daca portul
 * primeste conexiuni (iesire 0 = da). -w: limita de timp la conectare si la inactivitate. Iesire 1 la eroare de retea */
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include "netutil.h"

static void usage(void){
    fprintf(stderr, "uz: nc [-u] [-z] [-w secunde] gazda port | nc -l [-u] [-p port] [-w secunde] [port]\n");
}

static int write_all(int fd, const char *p, size_t n){
    while(n){
        ssize_t w = write(fd, p, n);
        if(w <= 0){
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

/* Copiaza intre stdin/stdout si fd. peer != NULL: UDP (datagrame spre peer) */
static int relay(int fd, int udp, struct sockaddr_in *peer, int idle_ms){
    char buf[2048];
    int in_open = 1;
    for(;;){
        struct pollfd p[2];
        int np = 0;
        if(in_open){
            p[np].fd = 0;
            p[np].events = POLLIN;
            p[np].revents = 0;
            np++;
        }
        p[np].fd = fd;
        p[np].events = POLLIN;
        p[np].revents = 0;
        int si = np++;
        int r = poll(p, (nfds_t)np, idle_ms);
        if(r < 0){
            if(errno == EINTR){
                return 0;
            }
            perror("nc: poll");
            return 1;
        }
        if(r == 0){
            return 0;                       /* inactiv prea mult (-w) */
        }
        if(p[si].revents & (POLLIN | POLLHUP | POLLERR)){
            ssize_t n;
            if(udp){
                struct sockaddr_in from;
                socklen_t fl = sizeof(from);
                n = recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            } else {
                n = recv(fd, buf, sizeof(buf), 0);
            }
            if(n < 0){
                if(errno == ECONNRESET || errno == ECONNREFUSED){
                    return 1;
                }
                perror("nc: recv");
                return 1;
            }
            if(n == 0){
                return 0;                   /* partenerul a inchis */
            }
            if(write_all(1, buf, (size_t)n) < 0){
                return 1;
            }
        }
        if(in_open && (p[0].revents & (POLLIN | POLLHUP))){
            ssize_t n = read(0, buf, sizeof(buf));
            if(n <= 0){
                in_open = 0;
                if(udp){
                    if(idle_ms < 0){
                        idle_ms = 2000;     /* UDP nu are FIN: mai asteptam 2 s raspunsuri, apoi iesim */
                    }
                } else {
                    shutdown(fd, SHUT_WR);
                }
                continue;
            }
            ssize_t off = 0;
            while(off < n){
                ssize_t w = udp && peer ? sendto(fd, buf + off, (size_t)(n - off), 0, (struct sockaddr *)peer, sizeof(*peer))
                                        : send(fd, buf + off, (size_t)(n - off), MSG_NOSIGNAL);
                if(w <= 0){
                    perror("nc: send");
                    return 1;
                }
                off += w;
            }
        }
    }
}

int main(int argc, char **argv){
    int listen_mode = 0, udp = 0, zero = 0;
    unsigned long wait_s = 0, port = 0;
    const char *host = NULL;
    const char *pos[3];
    int npos = 0;
    for(int i = 1; i < argc; i++){
        const char *a = argv[i];
        if(!strcmp(a, "--help")){
            puts("uz: nc [-u] [-z] [-w secunde] gazda port | nc -l [-u] [-p port] [-w secunde] [port]");
            return 0;
        }
        if(!strcmp(a, "-l")){
            listen_mode = 1;
        } else if(!strcmp(a, "-u")){
            udp = 1;
        } else if(!strcmp(a, "-z")){
            zero = 1;
        } else if(!strcmp(a, "-w") || !strcmp(a, "-p")){
            unsigned long v;
            if(i + 1 >= argc || !nu_parse_uint(argv[i + 1], a[1] == 'p' ? 65535 : 100000, &v)){
                fprintf(stderr, "nc: valoare gresita pentru %s\n", a);
                return 2;
            }
            i++;
            if(a[1] == 'w'){
                wait_s = v;
            } else {
                port = v;
            }
        } else if(a[0] == '-' && a[1]){
            fprintf(stderr, "nc: optiune necunoscuta %s\n", a);
            return 2;
        } else if(npos < 3){
            pos[npos++] = a;
        } else {
            usage();
            return 2;
        }
    }
    int idle_ms = wait_s ? (int)(wait_s * 1000) : -1;
    if(listen_mode){
        if(npos == 1 && !port){
            if(!nu_parse_uint(pos[0], 65535, &port)){
                usage();
                return 2;
            }
        } else if(npos != 0 || !port){
            usage();
            return 2;
        }
        int ls = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
        if(ls < 0){
            perror("nc: socket");
            return 1;
        }
        int one = 1;
        setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((unsigned short)port);
        if(bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0){
            perror("nc: bind");
            return 1;
        }
        if(udp){
            /* primul pachet decide partenerul; apoi relay (pachetul e afisat) */
            char first[2048];
            struct sockaddr_in peer;
            socklen_t pl = sizeof(peer);
            if(idle_ms >= 0){
                struct pollfd p = {ls, POLLIN, 0};
                if(poll(&p, 1, idle_ms) <= 0){
                    return 1;
                }
            }
            ssize_t n = recvfrom(ls, first, sizeof(first), 0, (struct sockaddr *)&peer, &pl);
            if(n < 0){
                perror("nc: recvfrom");
                return 1;
            }
            if(n > 0 && write_all(1, first, (size_t)n) < 0){
                return 1;
            }
            if(connect(ls, (struct sockaddr *)&peer, sizeof(peer)) < 0){
                perror("nc: connect");
                return 1;
            }
            return relay(ls, 1, NULL, idle_ms);
        }
        if(listen(ls, 1) < 0){
            perror("nc: listen");
            return 1;
        }
        if(idle_ms >= 0){
            struct pollfd p = {ls, POLLIN, 0};
            if(poll(&p, 1, idle_ms) <= 0){
                return 1;
            }
        }
        int cs = accept(ls, NULL, NULL);
        if(cs < 0){
            perror("nc: accept");
            return 1;
        }
        close(ls);
        return relay(cs, 0, NULL, idle_ms);
    }
    if(npos != 2){
        usage();
        return 2;
    }
    host = pos[0];
    if(!nu_parse_uint(pos[1], 65535, &port) || !port){
        fprintf(stderr, "nc: port gresit: %s\n", pos[1]);
        return 2;
    }
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((unsigned short)port);
    if(nu_resolve(host, &to.sin_addr) < 0){
        fprintf(stderr, "nc: %s: nume necunoscut\n", host);
        return 2;
    }
    int fd = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);
    if(fd < 0){
        perror("nc: socket");
        return 1;
    }
    if(wait_s){
        struct timeval tv = {(long)wait_s, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }
    if(connect(fd, (struct sockaddr *)&to, sizeof(to)) < 0){
        fprintf(stderr, "nc: conectare la %s port %lu (%s) esuata: %s\n", host, port, udp ? "udp" : "tcp", strerror(errno));
        return 1;
    }
    if(zero){
        fprintf(stderr, "Conexiune la %s port %lu [%s] reusita!\n", host, port, udp ? "udp" : "tcp");
        close(fd);
        return 0;
    }
    return relay(fd, udp, NULL, idle_ms);
}

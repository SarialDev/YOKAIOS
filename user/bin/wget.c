/* wget: HTTP/1.0 GET intr-un fisier (T9.5). Fara TLS (https:// e refuzat), fara chunked (cererea e HTTP/1.0).
 * uz: wget [-q] [-O fisier|-] [-T secunde] url        (url: [http://]gazda[:port][/cale])
 * Urmareste cel mult 5 redirectari (301/302/303/307/308) spre http. Fara -O fisierul poarta ultimul segment al caii
 * (index.html pentru "/"); un fisier existent e rescris. Un raspuns care nu e 2xx nu creeaza fisier (iesire 1) */
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/time.h>
#include "netutil.h"

#define HDR_MAX 8192

static int quiet;

struct url {
    char host[128];
    char port[8];
    char path[512];
};

static int parse_url(const char *s, struct url *u){
    if(!strncasecmp(s, "http://", 7)){
        s += 7;
    } else if(!strncasecmp(s, "https://", 8)){
        fprintf(stderr, "wget: https nu e suportat (fara TLS)\n");
        return -1;
    } else if(strstr(s, "://")){
        fprintf(stderr, "wget: schema necunoscuta: %s\n", s);
        return -1;
    }
    const char *slash = strchr(s, '/');
    size_t hl = slash ? (size_t)(slash - s) : strlen(s);
    if(hl == 0 || hl >= sizeof(u->host)){
        fprintf(stderr, "wget: adresa gresita\n");
        return -1;
    }
    char hp[128];
    memcpy(hp, s, hl);
    hp[hl] = 0;
    strcpy(u->port, "80");
    char *colon = strrchr(hp, ':');
    if(colon){
        *colon = 0;
        unsigned long pv;
        if(!nu_parse_uint(colon + 1, 65535, &pv) || pv == 0){
            fprintf(stderr, "wget: port gresit\n");
            return -1;
        }
        snprintf(u->port, sizeof(u->port), "%lu", pv);
    }
    strcpy(u->host, hp);
    if(slash){
        if(strlen(slash) >= sizeof(u->path)){
            fprintf(stderr, "wget: calea e prea lunga\n");
            return -1;
        }
        strcpy(u->path, slash);
    } else {
        strcpy(u->path, "/");
    }
    return 0;
}

/* Conecteaza la gazda:port; fd sau -1 */
static int dial(const struct url *u, int timeout_s, char *ipstr, size_t ipn){
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int g = getaddrinfo(u->host, u->port, &hints, &res);
    if(g != 0 || !res){
        fprintf(stderr, "wget: nu pot rezolva %s: %s\n", u->host, gai_strerror(g));
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if(fd < 0){
        perror("wget: socket");
        freeaddrinfo(res);
        return -1;
    }
    struct timeval tv = {timeout_s, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    snprintf(ipstr, ipn, "%s", inet_ntoa(((struct sockaddr_in *)res->ai_addr)->sin_addr));
    if(!quiet){
        fprintf(stderr, "Conectare la %s (%s):%s... ", u->host, ipstr, u->port);
    }
    if(connect(fd, res->ai_addr, res->ai_addrlen) < 0){
        fprintf(stderr, "%seroare: %s\n", quiet ? "wget: " : "", strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    if(!quiet){
        fprintf(stderr, "conectat.\n");
    }
    return fd;
}

static int send_all(int fd, const char *p, size_t n){
    while(n){
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if(w <= 0){
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
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

/* Valoarea antetului `name` (fara majuscule) din blocul de antete, sau NULL */
static const char *header(const char *hdrs, const char *name, char *out, size_t cap){
    size_t nl = strlen(name);
    for(const char *l = strchr(hdrs, '\n'); l; l = strchr(l + 1, '\n')){
        l++;
        if(!strncasecmp(l, name, nl) && l[nl] == ':'){
            const char *v = l + nl + 1;
            while(*v == ' ' || *v == '\t'){
                v++;
            }
            size_t k = 0;
            while(v[k] && v[k] != '\r' && v[k] != '\n' && k < cap - 1){
                out[k] = v[k];
                k++;
            }
            out[k] = 0;
            return out;
        }
    }
    return NULL;
}

static const char *base_name(const struct url *u, char *buf, size_t cap){
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", u->path);
    char *q = strchr(tmp, '?');
    if(q){
        *q = 0;
    }
    q = strchr(tmp, '#');
    if(q){
        *q = 0;
    }
    char *s = strrchr(tmp, '/');
    const char *n = s ? s + 1 : tmp;
    snprintf(buf, cap, "%s", *n ? n : "index.html");
    return buf;
}

int main(int argc, char **argv){
    const char *out_name = NULL, *arg = NULL;
    unsigned long timeout_s = 30;
    for(int i = 1; i < argc; i++){
        if(!strcmp(argv[i], "--help")){
            puts("uz: wget [-q] [-O fisier|-] [-T secunde] url");
            return 0;
        } else if(!strcmp(argv[i], "-q")){
            quiet = 1;
        } else if(!strcmp(argv[i], "-O") && i + 1 < argc){
            out_name = argv[++i];
        } else if(!strcmp(argv[i], "-T") && i + 1 < argc){
            if(!nu_parse_uint(argv[++i], 3600, &timeout_s) || !timeout_s){
                fprintf(stderr, "wget: timeout gresit\n");
                return 2;
            }
        } else if(argv[i][0] == '-' && argv[i][1]){
            fprintf(stderr, "wget: optiune necunoscuta %s\n", argv[i]);
            return 2;
        } else if(!arg){
            arg = argv[i];
        } else {
            fprintf(stderr, "uz: wget [-q] [-O fisier|-] [-T secunde] url\n");
            return 2;
        }
    }
    if(!arg){
        fprintf(stderr, "uz: wget [-q] [-O fisier|-] [-T secunde] url\n");
        return 2;
    }
    struct url u;
    if(parse_url(arg, &u) < 0){
        return 1;
    }

    static char buf[HDR_MAX + 1];
    for(int hop = 0;; hop++){
        char ip[20];
        int fd = dial(&u, (int)timeout_s, ip, sizeof(ip));
        if(fd < 0){
            return 1;
        }
        char req[1024];
        const char *hostport_sep = strcmp(u.port, "80") ? ":" : "";
        int rl = snprintf(req, sizeof(req),
                          "GET %s HTTP/1.0\r\nHost: %s%s%s\r\nUser-Agent: Yokai-wget/1.0\r\nAccept: */*\r\nConnection: close\r\n\r\n",
                          u.path, u.host, hostport_sep, strcmp(u.port, "80") ? u.port : "");
        if(rl >= (int)sizeof(req) || send_all(fd, req, (size_t)rl) < 0){
            fprintf(stderr, "wget: trimiterea cererii a esuat\n");
            close(fd);
            return 1;
        }
        if(!quiet){
            fprintf(stderr, "cerere HTTP trimisa, astept raspunsul... ");
        }
        /* antetele: pana la \r\n\r\n */
        size_t have = 0;
        char *body = NULL;
        while(!body){
            if(have >= HDR_MAX){
                fprintf(stderr, "wget: antete prea mari\n");
                close(fd);
                return 1;
            }
            ssize_t n = recv(fd, buf + have, HDR_MAX - have, 0);
            if(n <= 0){
                fprintf(stderr, "wget: %s inainte de sfarsitul antetelor\n", n < 0 ? strerror(errno) : "conexiune inchisa");
                close(fd);
                return 1;
            }
            have += (size_t)n;
            buf[have] = 0;
            char *e = strstr(buf, "\r\n\r\n");
            if(e){
                *e = 0;
                body = e + 4;
            }
        }
        size_t body_have = have - (size_t)(body - buf);
        int code = 0;
        const char *reason = "";
        if(strncmp(buf, "HTTP/1.", 7) || buf[8] != ' '){
            fprintf(stderr, "wget: raspuns HTTP invalid\n");
            close(fd);
            return 1;
        }
        code = atoi(buf + 9);
        const char *sp = buf + 9;
        while(*sp && *sp != ' ' && *sp != '\r'){
            sp++;
        }
        reason = *sp == ' ' ? sp + 1 : "";
        char status_line[96];
        size_t k = 0;
        while(reason[k] && reason[k] != '\r' && reason[k] != '\n' && k < sizeof(status_line) - 1){
            status_line[k] = reason[k];
            k++;
        }
        status_line[k] = 0;
        if(!quiet){
            fprintf(stderr, "%d %s\n", code, status_line);
        }
        if(code == 301 || code == 302 || code == 303 || code == 307 || code == 308){
            char loc[512];
            if(!header(buf, "Location", loc, sizeof(loc))){
                fprintf(stderr, "wget: redirectare fara Location\n");
                close(fd);
                return 1;
            }
            close(fd);
            if(hop >= 5){
                fprintf(stderr, "wget: prea multe redirectari\n");
                return 1;
            }
            if(loc[0] == '/'){
                snprintf(u.path, sizeof(u.path), "%s", loc);
            } else {
                struct url nu;
                if(parse_url(loc, &nu) < 0){
                    return 1;
                }
                u = nu;
            }
            if(!quiet){
                fprintf(stderr, "Redirectat la http://%s:%s%s\n", u.host, u.port, u.path);
            }
            continue;
        }
        if(code < 200 || code > 299){
            fprintf(stderr, "wget: eroare HTTP %d %s\n", code, status_line);
            close(fd);
            return 1;
        }
        char tmp[64];
        if(header(buf, "Transfer-Encoding", tmp, sizeof(tmp)) && !strncasecmp(tmp, "chunked", 7)){
            fprintf(stderr, "wget: Transfer-Encoding chunked nu e suportat\n");
            close(fd);
            return 1;
        }
        long long clen = -1;
        if(header(buf, "Content-Length", tmp, sizeof(tmp))){
            clen = atoll(tmp);
        }
        char namebuf[256];
        const char *name = out_name ? out_name : base_name(&u, namebuf, sizeof(namebuf));
        int to_stdout = !strcmp(name, "-");
        int ofd = to_stdout ? 1 : open(name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if(ofd < 0){
            fprintf(stderr, "wget: %s: %s\n", name, strerror(errno));
            close(fd);
            return 1;
        }
        if(!quiet){
            if(clen >= 0){
                fprintf(stderr, "Lungime: %lld\n", clen);
            } else {
                fprintf(stderr, "Lungime: necunoscuta\n");
            }
            fprintf(stderr, "Salvez in: '%s'\n", to_stdout ? "stdout" : name);
        }
        long long total = 0;
        int bad = 0;
        if(body_have){
            if(write_all(ofd, body, body_have) < 0){
                bad = 1;
            }
            total += (long long)body_have;
        }
        while(!bad){
            ssize_t n = recv(fd, buf, HDR_MAX, 0);
            if(n < 0){
                fprintf(stderr, "wget: citire: %s\n", strerror(errno));
                bad = 1;
                break;
            }
            if(n == 0){
                break;
            }
            if(write_all(ofd, buf, (size_t)n) < 0){
                fprintf(stderr, "wget: scriere: %s\n", strerror(errno));
                bad = 1;
                break;
            }
            total += n;
        }
        close(fd);
        if(!to_stdout){
            close(ofd);
        }
        if(!bad && clen >= 0 && total != clen){
            fprintf(stderr, "wget: transfer incomplet (%lld din %lld octeti)\n", total, clen);
            bad = 1;
        }
        if(bad){
            return 1;
        }
        if(!quiet){
            fprintf(stderr, "'%s' salvat [%lld]\n", to_stdout ? "stdout" : name, total);
        }
        return 0;
    }
}

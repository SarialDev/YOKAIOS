/* Teste T9.3: socket-uri BSD (AF_INET TCP/UDP), poll/select peste socket-uri + pipe + consola, EINTR, SO_RCVTIMEO.
 * Pornit de kernel cu test=sockets ip=... sockhost=<port> (kernel/tests/test_sockets.cpp); argv[1] = portul serverului
 * de pe gazda, la care ne conectam prin 10.0.2.2. Faza A: API fara retea; faza B: server echo TCP+UDP pe portul 7,
 * la care tests/sockets_host.py se conecteaza prin hostfwd; faza C: client TCP spre gazda. Iesire 0 = totul bine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; printf("socktest: FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); fflush(stdout); } \
} while(0)

static long now_ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void msleep(long ms){
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&ts, NULL);
}

static struct sockaddr_in make_addr(const char *ip, int port){
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = ip ? inet_addr(ip) : htonl(INADDR_ANY);
    return a;
}

static volatile int got_usr1;
static void on_usr1(int sig){
    (void)sig;
    got_usr1++;
}

/* Un copil care, dupa ms, trimite SIGUSR1 parintelui (fara SA_RESTART: apelul blocat trebuie sa dea EINTR) */
static pid_t signal_later(long ms){
    pid_t parent = getpid();
    pid_t c = fork();
    if(c == 0){
        msleep(ms);
        kill(parent, SIGUSR1);
        _exit(0);
    }
    return c;
}

/* --- faza A: API si poll/select fara retea --- */
static void phase_a(void){
    errno = 0;
    CHECK(socket(1, SOCK_STREAM, 0) < 0 && errno == EAFNOSUPPORT);       /* AF_UNIX: nu (inca) */
    errno = 0;
    CHECK(socket(AF_INET, 3, 0) < 0 && errno == EPROTONOSUPPORT);        /* SOCK_RAW */
    char b[8];
    errno = 0;
    CHECK(send(77, b, 1, 0) < 0 && errno == EBADF);
    int p[2];
    CHECK(pipe(p) == 0);
    errno = 0;
    CHECK(recv(p[0], b, 1, 0) < 0 && errno == ENOTSOCK);
    errno = 0;
    CHECK(listen(p[0], 1) < 0 && errno == ENOTSOCK);

    /* UDP: optiuni, bind, getsockname, timeout de citire, neblocant */
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(u >= 0);
    int v = 0;
    socklen_t vl = sizeof(v);
    CHECK(getsockopt(u, SOL_SOCKET, SO_TYPE, &v, &vl) == 0 && v == SOCK_DGRAM);
    int one = 1;
    CHECK(setsockopt(u, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    CHECK(setsockopt(u, SOL_SOCKET, 9999, &one, sizeof(one)) < 0 && errno == ENOPROTOOPT);
    struct sockaddr_in a = make_addr(NULL, 5001);
    CHECK(bind(u, (struct sockaddr *)&a, sizeof(a)) == 0);
    struct sockaddr_in got;
    socklen_t gl = sizeof(got);
    memset(&got, 0, sizeof(got));
    CHECK(getsockname(u, (struct sockaddr *)&got, &gl) == 0 && gl == sizeof(got) && got.sin_family == AF_INET &&
          ntohs(got.sin_port) == 5001);
    struct timeval tv = {0, 150000};
    CHECK(setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == 0);
    struct timeval back;
    socklen_t bl = sizeof(back);
    CHECK(getsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &back, &bl) == 0 && back.tv_sec == 0 && back.tv_usec == 150000);
    long t0 = now_ms();
    errno = 0;
    CHECK(recv(u, b, sizeof(b), 0) < 0 && errno == EAGAIN);
    long dt = now_ms() - t0;
    CHECK(dt >= 100 && dt < 2000);
    printf("socktest: SO_RCVTIMEO 150 ms -> %ld ms\n", dt);
    errno = 0;
    CHECK(recv(u, b, sizeof(b), MSG_DONTWAIT) < 0 && errno == EAGAIN);
    int fl = fcntl(u, F_GETFL);
    CHECK(fcntl(u, F_SETFL, fl | O_NONBLOCK) == 0);
    tv.tv_sec = tv.tv_usec = 0;
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    t0 = now_ms();
    errno = 0;
    CHECK(recv(u, b, sizeof(b), 0) < 0 && errno == EAGAIN);
    CHECK(now_ms() - t0 < 100);
    CHECK(fcntl(u, F_SETFL, fl) == 0);
    struct pollfd pf = {u, POLLIN, 0};
    t0 = now_ms();
    CHECK(poll(&pf, 1, 60) == 0 && pf.revents == 0);
    dt = now_ms() - t0;
    CHECK(dt >= 50 && dt < 1000);
    pf.events = POLLOUT;
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLOUT));

    /* Adresa folosita: al doilea bind pe un port de TCP aflat in listen */
    int l1 = socket(AF_INET, SOCK_STREAM, 0);
    int l2 = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(l1 >= 0 && l2 >= 0);
    a = make_addr(NULL, 7000);
    CHECK(bind(l1, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(l1, 4) == 0);
    errno = 0;
    CHECK(bind(l2, (struct sockaddr *)&a, sizeof(a)) < 0 && errno == EADDRINUSE);
    close(l2);

    /* poll / select pe pipe */
    pf = (struct pollfd){p[0], POLLIN, 0};
    CHECK(poll(&pf, 1, 0) == 0);
    struct pollfd pw = {p[1], POLLOUT, 0};
    CHECK(poll(&pw, 1, 0) == 1 && (pw.revents & POLLOUT));
    CHECK(write(p[1], "xy", 2) == 2);
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN));
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(p[0], &rs);
    FD_SET(u, &rs);
    struct timeval z = {0, 0};
    CHECK(select(p[0] > u ? p[0] + 1 : u + 1, &rs, NULL, NULL, &z) == 1 && FD_ISSET(p[0], &rs) && !FD_ISSET(u, &rs));
    CHECK(read(p[0], b, 8) == 2);
    CHECK(poll(&pf, 1, 0) == 0);
    close(p[1]);
    CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLHUP));
    CHECK(read(p[0], b, 8) == 0);
    close(p[0]);
    struct pollfd bad[2] = {{77, POLLIN, 0}, {-1, POLLIN, 0}};
    CHECK(poll(bad, 2, 0) == 1 && bad[0].revents == POLLNVAL && bad[1].revents == 0);
    struct pollfd con = {1, POLLOUT, 0};
    CHECK(poll(&con, 1, 0) == 1 && (con.revents & POLLOUT));

    /* poll amestecat socket + pipe: un copil scrie in pipe dupa 80 ms */
    CHECK(pipe(p) == 0);
    pid_t c = fork();
    if(c == 0){
        close(p[0]);
        msleep(80);
        write(p[1], "z", 1);
        _exit(0);
    }
    close(p[1]);
    struct pollfd mix[2] = {{u, POLLIN, 0}, {p[0], POLLIN, 0}};
    t0 = now_ms();
    CHECK(poll(mix, 2, 5000) == 1 && mix[0].revents == 0 && (mix[1].revents & POLLIN));
    dt = now_ms() - t0;
    CHECK(dt >= 60 && dt < 2000);
    printf("socktest: poll socket+pipe -> %ld ms\n", dt);
    waitpid(c, NULL, 0);
    close(p[0]);

    /* semnalul trezeste apelurile blocate: accept pe un listen fara conexiuni, poll cu timeout lung */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_usr1;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    got_usr1 = 0;
    c = signal_later(100);
    t0 = now_ms();
    errno = 0;
    CHECK(accept(l1, NULL, NULL) < 0 && errno == EINTR && got_usr1 == 1);
    CHECK(now_ms() - t0 < 3000);
    waitpid(c, NULL, 0);
    c = signal_later(100);
    mix[0].revents = 0;
    errno = 0;
    CHECK(poll(mix, 1, 20000) < 0 && errno == EINTR && got_usr1 == 2);
    waitpid(c, NULL, 0);
    c = signal_later(100);
    errno = 0;
    CHECK(recv(u, b, sizeof(b), 0) < 0 && errno == EINTR && got_usr1 == 3);
    waitpid(c, NULL, 0);
    close(l1);
    close(u);
    printf("socktest: [OK] faza A (api, poll/select, EINTR)\n");
}

/* --- faza B: server echo --- */
#define MAXC 8
#define TCP_CLIENTS 2
#define UDP_DGRAMS 3

static void phase_b(void){
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    CHECK(ls >= 0 && us >= 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = make_addr(NULL, 7);
    CHECK(bind(ls, (struct sockaddr *)&a, sizeof(a)) == 0);
    CHECK(listen(ls, 4) == 0);
    CHECK(bind(us, (struct sockaddr *)&a, sizeof(a)) == 0);
    printf("socktest: astept host (tcp+udp pe portul 7)\n");
    fflush(stdout);

    int clients[MAXC];
    long received[MAXC];
    int nclients = 0, done_clients = 0, udp_done = 0;
    long total = 0;
    static char buf[4096];
    long deadline = now_ms() + 90000;
    while((done_clients < TCP_CLIENTS || udp_done < UDP_DGRAMS) && now_ms() < deadline){
        struct pollfd pf[2 + MAXC];
        int n = 0;
        pf[n++] = (struct pollfd){ls, POLLIN, 0};
        pf[n++] = (struct pollfd){us, POLLIN, 0};
        for(int i = 0; i < nclients; i++){
            pf[n++] = (struct pollfd){clients[i], POLLIN, 0};
        }
        int r = poll(pf, (nfds_t)n, 1000);
        CHECK(r >= 0);
        if(r <= 0){
            continue;
        }
        if(pf[0].revents & POLLIN){
            struct sockaddr_in peer;
            socklen_t pl = sizeof(peer);
            int c = accept(ls, (struct sockaddr *)&peer, &pl);
            CHECK(c >= 0 && pl == sizeof(peer));
            if(c >= 0 && nclients < MAXC){
                printf("socktest: accept de la %s\n", inet_ntoa(peer.sin_addr));
                CHECK(strcmp(inet_ntoa(peer.sin_addr), "10.0.2.2") == 0);
                struct sockaddr_in me;
                socklen_t ml = sizeof(me);
                CHECK(getsockname(c, (struct sockaddr *)&me, &ml) == 0 && ntohs(me.sin_port) == 7);
                struct sockaddr_in pr;
                socklen_t prl = sizeof(pr);
                CHECK(getpeername(c, (struct sockaddr *)&pr, &prl) == 0 && pr.sin_port == peer.sin_port);
                int e = -1;
                socklen_t el = sizeof(e);
                CHECK(getsockopt(c, SOL_SOCKET, SO_ERROR, &e, &el) == 0 && e == 0);
                received[nclients] = 0;
                clients[nclients++] = c;
            }
        }
        if(pf[1].revents & POLLIN){
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t m = recvfrom(us, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fl);
            CHECK(m > 0 && fl == sizeof(from));
            CHECK(sendto(us, buf, (size_t)m, 0, (struct sockaddr *)&from, fl) == m);
            udp_done++;
        }
        for(int i = 0; i < nclients; i++){
            if(clients[i] < 0 || !(pf[2 + i].revents & (POLLIN | POLLHUP | POLLERR))){
                continue;
            }
            ssize_t m = recv(clients[i], buf, sizeof(buf), 0);
            if(m <= 0){
                CHECK(m == 0);
                printf("socktest: client %d: %ld octeti\n", i, received[i]);
                total += received[i];
                close(clients[i]);
                clients[i] = -1;
                done_clients++;
                continue;
            }
            received[i] += m;
            ssize_t off = 0;
            while(off < m){
                ssize_t w = send(clients[i], buf + off, (size_t)(m - off), 0);
                CHECK(w > 0);
                if(w <= 0){
                    break;
                }
                off += w;
            }
        }
    }
    CHECK(done_clients == TCP_CLIENTS && udp_done == UDP_DGRAMS);
    CHECK(total == 2 * 100000);
    close(ls);
    close(us);
    printf("socktest: [OK] faza B (server echo: %d clienti TCP, %ld octeti, %d datagrame UDP)\n", done_clients, total,
           udp_done);
}

/* --- faza C: client spre serverul de pe gazda --- */
static void phase_c(int port){
    struct sockaddr_in host = make_addr("10.0.2.2", port);
    int s = -1;
    /* connect neblocant: EINPROGRESS, poll(POLLOUT), SO_ERROR; reincercam pana ascultatorul e gata */
    for(int attempt = 0; attempt < 20; attempt++){
        s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
        CHECK(s >= 0);
        errno = 0;
        int r = connect(s, (struct sockaddr *)&host, sizeof(host));
        if(r < 0 && errno == EINPROGRESS){
            struct pollfd pf = {s, POLLOUT, 0};
            CHECK(poll(&pf, 1, 10000) == 1);
            int e = 0;
            socklen_t el = sizeof(e);
            CHECK(getsockopt(s, SOL_SOCKET, SO_ERROR, &e, &el) == 0);
            r = e ? -1 : 0;
        }
        if(r == 0){
            break;
        }
        close(s);
        s = -1;
        msleep(500);
    }
    CHECK(s >= 0);
    if(s < 0){
        return;
    }
    int fl = fcntl(s, F_GETFL);
    CHECK(fcntl(s, F_SETFL, fl & ~O_NONBLOCK) == 0);
    int one = 1;
    CHECK(setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0);
    struct timeval tv = {20, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    static const char msg[] = "hello yokai\n";
    CHECK(send(s, msg, sizeof(msg) - 1, 0) == (ssize_t)sizeof(msg) - 1);
    char buf[128];
    int n = 0;
    while(n < (int)sizeof(msg) - 1){
        ssize_t m = recv(s, buf + n, sizeof(buf) - (size_t)n, 0);
        if(m <= 0){
            break;
        }
        n += (int)m;
    }
    CHECK(n == (int)sizeof(msg) - 1 && memcmp(buf, "HELLO YOKAI\n", 12) == 0);
    /* shutdown(WR) -> gazda vede EOF, raspunde "bye" si inchide; noi citim "bye" si apoi 0 */
    CHECK(shutdown(s, SHUT_WR) == 0);
    n = 0;
    for(;;){
        ssize_t m = recv(s, buf + n, sizeof(buf) - (size_t)n, 0);
        if(m <= 0){
            CHECK(m == 0);
            break;
        }
        n += (int)m;
    }
    CHECK(n == 4 && memcmp(buf, "bye\n", 4) == 0);
    close(s);

    /* un port inchis pe gazda: connect trebuie sa esueze (refuz / reset / timp depasit), nu sa blocheze */
    s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in closed = make_addr("10.0.2.2", 1);
    struct timeval st = {8, 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
    long t0 = now_ms();
    errno = 0;
    CHECK(connect(s, (struct sockaddr *)&closed, sizeof(closed)) < 0);
    printf("socktest: connect pe port inchis -> errno %d dupa %ld ms\n", errno, now_ms() - t0);
    close(s);

    /* netdb: rezolvare numerica */
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    CHECK(getaddrinfo("10.0.2.2", "echo", &hints, &res) == 0 && res && res->ai_family == AF_INET);
    if(res){
        struct sockaddr_in *ra = (struct sockaddr_in *)res->ai_addr;
        CHECK(ntohs(ra->sin_port) == 7 && ra->sin_addr.s_addr == inet_addr("10.0.2.2") && res->ai_next == NULL);
        freeaddrinfo(res);
    }
    CHECK(getaddrinfo("nu-exista.example", NULL, &hints, &res) == EAI_NONAME);
    struct hostent *h = gethostbyname("localhost");
    CHECK(h && h->h_length == 4 && memcmp(h->h_addr, "\x7f\0\0\x01", 4) == 0);
    char txt[INET_ADDRSTRLEN];
    struct in_addr ia;
    CHECK(inet_pton(AF_INET, "192.168.1.200", &ia) == 1 && inet_ntop(AF_INET, &ia, txt, sizeof(txt)) &&
          strcmp(txt, "192.168.1.200") == 0 && ntohl(ia.s_addr) == 0xC0A801C8);
    CHECK(inet_pton(AF_INET, "1.2.3", &ia) == 0 && inet_pton(AF_INET, "256.1.1.1", &ia) == 0 &&
          inet_addr("x") == INADDR_NONE);
    printf("socktest: [OK] faza C (client, connect neblocant, shutdown, netdb)\n");
}

int main(int argc, char **argv){
    setvbuf(stdout, NULL, _IONBF, 0);
    int port = argc > 1 ? atoi(argv[1]) : 0;
    phase_a();
    if(port > 0){
        phase_b();
        phase_c(port);
    } else {
        printf("socktest: fara retea (fara portul gazdei): fazele B si C sar peste\n");
    }
    printf("socktest: %d verificari, %d esuate\n", checks, failed);
    if(failed){
        return 1;
    }
    printf("[OK] socktest\n");
    return 0;
}

/* Teste T9.5: utilitarele de retea /bin/{ping,ifconfig,netstat,wget,nc}, pornite cu fork+execve. Pornit de kernel
 * (kernel/tests/test_netutils.cpp); argv[1] = portul serverului HTTP de pe gazda, argv[2] = portul serverului echo TCP+UDP
 * (tests/netutils_host.py, ajunse ca 10.0.2.2:<port>). Faza A nu are nevoie de gazda; faza B asteapta gazda ("astept host"):
 * aceasta se conecteaza prin hostfwd la `nc -l 7` din oaspete. Iesire 0 = totul bine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; printf("nettest: FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); fflush(stdout); } \
} while(0)

#define OUTCAP 262144
static char *out;               /* iesirea (stdout + stderr) a ultimului program, terminata cu 0 */
static long out_len;

static void load_file(const char *path){
    int fd = open(path, O_RDONLY);
    out_len = 0;
    out[0] = 0;
    if(fd < 0){
        return;
    }
    int r;
    while(out_len < OUTCAP - 1 && (r = (int)read(fd, out + out_len, (size_t)(OUTCAP - 1 - out_len))) > 0){
        out_len += r;
    }
    close(fd);
    out[out_len] = 0;
}

/* Porneste /bin/<a0> cu stdin = input (sau /dev/null) si stdout+stderr in fisierul output; intoarce pid-ul */
static pid_t spawn(const char *input, const char *output, const char *a0, ...){
    char *av[10];
    va_list ap;
    int n = 0;
    va_start(ap, a0);
    for(const char *a = a0; a && n < 8; a = va_arg(ap, const char *)){
        av[n++] = (char *)a;
    }
    va_end(ap);
    av[n] = NULL;
    pid_t pid = fork();
    if(pid == 0){
        int in = open(input ? input : "/dev/null", O_RDONLY);
        int o = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if(in < 0 || o < 0){
            _exit(126);
        }
        dup2(in, 0);
        dup2(o, 1);
        dup2(o, 2);
        close(in);
        close(o);
        char path[64];
        snprintf(path, sizeof(path), "/bin/%s", a0);
        execve(path, av, environ);
        _exit(127);
    }
    return pid;
}

static int wait_for(pid_t pid){
    int status = 0;
    if(waitpid(pid, &status, 0) != pid){
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1000 + WTERMSIG(status);
}

/* Ruleaza si asteapta; iesirea ramane in `out` */
static int run(const char *input, const char *a0, ...){
    char *av[10];
    va_list ap;
    int n = 0;
    va_start(ap, a0);
    for(const char *a = a0; a && n < 8; a = va_arg(ap, const char *)){
        av[n++] = (char *)a;
    }
    va_end(ap);
    av[n] = NULL;
    pid_t pid = fork();
    if(pid < 0){
        return -1;
    }
    if(pid == 0){
        int in = open(input ? input : "/dev/null", O_RDONLY);
        int o = open("/tmp/nt_out", O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if(in < 0 || o < 0){
            _exit(126);
        }
        dup2(in, 0);
        dup2(o, 1);
        dup2(o, 2);
        close(in);
        close(o);
        char path[64];
        snprintf(path, sizeof(path), "/bin/%s", a0);
        execve(path, av, environ);
        _exit(127);
    }
    int code = wait_for(pid);
    load_file("/tmp/nt_out");
    return code;
}

static int has(const char *s){
    return strstr(out, s) != NULL;
}

static void put_file(const char *path, const void *data, size_t n){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd >= 0){
        write(fd, data, n);
        close(fd);
    }
}

static long file_size(const char *path){
    struct stat st;
    return stat(path, &st) < 0 ? -1 : (long)st.st_size;
}

/* Continutul unui fisier mic comparat cu text exact */
static int file_is(const char *path, const char *text){
    static char b[512];
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return 0;
    }
    int n = (int)read(fd, b, sizeof(b) - 1);
    close(fd);
    if(n < 0){
        return 0;
    }
    b[n] = 0;
    return strcmp(b, text) == 0;
}

static unsigned char pat(long i){
    return (unsigned char)((i * 131 + (i >> 8) * 7) & 255);
}

static void phase_a(void){
    /* ping */
    CHECK(run(NULL, "ping", "-c", "3", "10.0.2.2", NULL) == 0);
    CHECK(has("PING 10.0.2.2 (10.0.2.2)"));
    CHECK(has("icmp_seq=1 ") && has("icmp_seq=3 ") && has("ttl="));
    CHECK(has("3 pachete trimise, 3 primite, 0% pierdute"));
    CHECK(has("rtt min/avg/max/mdev = "));
    CHECK(run(NULL, "ping", "-c", "1", "-s", "1000", "10.0.2.2", NULL) == 0);
    CHECK(has("1008 octeti de la 10.0.2.2"));
    CHECK(run(NULL, "ping", "-c", "1", "-W", "1", "10.0.2.77", NULL) == 1);     /* nimeni nu raspunde la ARP */
    CHECK(has("1 pachete trimise, 0 primite, 100% pierdute"));
    CHECK(!has("rtt min"));
    CHECK(run(NULL, "ping", NULL) == 2);
    CHECK(run(NULL, "ping", "-c", "x", "10.0.2.2", NULL) == 2);
    CHECK(run(NULL, "ping", "-Q", "10.0.2.2", NULL) == 2);
    CHECK(run(NULL, "ping", "--help", NULL) == 0 && has("uz: ping"));

    /* ifconfig: afisare */
    CHECK(run(NULL, "ifconfig", "eth0", NULL) == 0);
    CHECK(has("eth0: flags=") && has("<UP,BROADCAST,RUNNING,MULTICAST>"));
    CHECK(has("inet 10.0.2.15  netmask 255.255.255.0  broadcast 10.0.2.255"));
    CHECK(has("ether ") && has("RX packets ") && has("TX packets "));
    CHECK(run(NULL, "ifconfig", NULL) == 0 && has("eth0: ") && has("inet 10.0.2.15"));
    CHECK(run(NULL, "ifconfig", "-a", NULL) == 0 && has("eth0: "));
    CHECK(run(NULL, "ifconfig", "nosuch0", NULL) == 1 && has("interfata necunoscuta"));
    CHECK(run(NULL, "ifconfig", "eth0", "999.1.1.1", NULL) == 1);
    CHECK(run(NULL, "ifconfig", "eth0", "10.0.2.9/40", NULL) == 1);
    /* ifconfig: adresa statica si gateway */
    CHECK(run(NULL, "ifconfig", "eth0", "10.0.2.16/24", NULL) == 0);
    CHECK(run(NULL, "ifconfig", "eth0", NULL) == 0 && has("inet 10.0.2.16  netmask 255.255.255.0  broadcast 10.0.2.255"));
    CHECK(run(NULL, "ifconfig", "eth0", "10.0.2.15", "netmask", "255.255.255.0", "gw", "10.0.2.2", NULL) == 0);
    CHECK(run(NULL, "ifconfig", "eth0", NULL) == 0 && has("inet 10.0.2.15  netmask 255.255.255.0"));
    CHECK(run(NULL, "ping", "-c", "1", "10.0.2.2", NULL) == 0);

    /* netstat */
    CHECK(run(NULL, "netstat", "-r", NULL) == 0);
    CHECK(has("Tabela de rutare IP") && has("0.0.0.0         10.0.2.2        0.0.0.0         UG"));
    CHECK(has("10.0.2.0        0.0.0.0         255.255.255.0   U"));
    CHECK(run(NULL, "netstat", NULL) == 0 && has("Proto") && has("Recv-Q"));
    CHECK(run(NULL, "netstat", "-a", "-t", NULL) == 0 && has("fara servere") == 0 && has("Stare"));
    CHECK(run(NULL, "netstat", "-x", NULL) == 2);
    CHECK(run(NULL, "netstat", "stuff", NULL) == 2);

    /* wget / nc: utilizare si erori fara gazda */
    CHECK(run(NULL, "wget", NULL) == 2);
    CHECK(run(NULL, "wget", "-Z", "x", NULL) == 2);
    CHECK(run(NULL, "wget", "https://10.0.2.2/", NULL) == 1 && has("TLS"));
    CHECK(run(NULL, "wget", "http://10.0.2.2:99999/", NULL) == 1);
    CHECK(run(NULL, "nc", NULL) == 2);
    CHECK(run(NULL, "nc", "10.0.2.2", "0", NULL) == 2);
    CHECK(run(NULL, "nc", "-l", NULL) == 2);
    CHECK(run(NULL, "nc", "-z", "-w", "3", "10.0.2.2", "1", NULL) == 1 && has("esuata"));
    printf("nettest: [OK] faza A (ping, ifconfig, netstat, utilizare wget/nc)\n");
    fflush(stdout);
}

static void phase_b(const char *http_port, const char *echo_port){
    char url[128], addr[64];

    /* nc -l 7 in fundal (gazda se conecteaza prin hostfwd); netstat il trebuie sa-l arate ca LISTEN */
    put_file("/tmp/nc_in", "pong from guest\n", 16);
    pid_t srv = spawn("/tmp/nc_in", "/tmp/nc_out", "nc", "-l", "-w", "90", "7", NULL);
    CHECK(srv > 0);
    int seen = 0;
    for(int i = 0; i < 100 && !seen; i++){
        run(NULL, "netstat", "-l", "-t", NULL);
        seen = has("0.0.0.0:7 ") && has("LISTEN");
        if(!seen){
            usleep(50000);
        }
    }
    CHECK(seen);
    CHECK(run(NULL, "netstat", "-t", NULL) == 0 && !has("0.0.0.0:7 "));           /* fara -a nu arata serverele */
    CHECK(run(NULL, "netstat", "-a", "-t", NULL) == 0 && has("0.0.0.0:7 ") && has("LISTEN"));
    printf("nettest: astept host\n");
    fflush(stdout);
    int rc = wait_for(srv);
    CHECK(rc == 0);
    CHECK(file_is("/tmp/nc_out", "ping from host\n"));
    printf("nettest: [OK] faza B (nc -l, netstat LISTEN)\n");
    fflush(stdout);

    /* wget */
    chdir("/tmp");
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/hello.txt", http_port);
    CHECK(run(NULL, "wget", "-q", "-O", "/tmp/h.txt", url, NULL) == 0);
    CHECK(file_is("/tmp/h.txt", "hello from host\n"));
    CHECK(run(NULL, "wget", url, NULL) == 0);
    CHECK(has("200 OK") && has("Lungime: 16") && has("hello.txt") && has("salvat [16]"));
    CHECK(file_is("/tmp/hello.txt", "hello from host\n"));
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/", http_port);
    CHECK(run(NULL, "wget", "-q", url, NULL) == 0);
    CHECK(file_is("/tmp/index.html", "index page\n"));
    snprintf(url, sizeof(url), "10.0.2.2:%s/hello.txt", http_port);               /* fara schema */
    CHECK(run(NULL, "wget", "-q", "-O", "-", url, NULL) == 0 && strcmp(out, "hello from host\n") == 0);
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/big.bin", http_port);
    CHECK(run(NULL, "wget", "-q", "-O", "/tmp/big.bin", url, NULL) == 0);
    CHECK(file_size("/tmp/big.bin") == 150000);
    {
        int fd = open("/tmp/big.bin", O_RDONLY);
        static unsigned char b[4096];
        long pos = 0;
        int ok = fd >= 0;
        int n;
        while(ok && (n = (int)read(fd, b, sizeof(b))) > 0){
            for(int i = 0; i < n; i++){
                if(b[i] != pat(pos + i)){
                    ok = 0;
                    break;
                }
            }
            pos += n;
        }
        if(fd >= 0){
            close(fd);
        }
        CHECK(ok && pos == 150000);
    }
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/redir", http_port);            /* 302 relativ */
    CHECK(run(NULL, "wget", "-O", "/tmp/r1.txt", url, NULL) == 0 && has("Redirectat la"));
    CHECK(file_is("/tmp/r1.txt", "hello from host\n"));
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/redir2", http_port);           /* 302 absolut */
    CHECK(run(NULL, "wget", "-q", "-O", "/tmp/r2.txt", url, NULL) == 0);
    CHECK(file_is("/tmp/r2.txt", "hello from host\n"));
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/loop", http_port);             /* redirectari fara sfarsit */
    CHECK(run(NULL, "wget", "-q", "-O", "/tmp/loop.txt", url, NULL) == 1);
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/missing", http_port);
    unlink("/tmp/missing");
    CHECK(run(NULL, "wget", url, NULL) == 1 && has("404"));
    CHECK(access("/tmp/missing", 0) != 0);
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/empty", http_port);
    CHECK(run(NULL, "wget", "-q", "-O", "/tmp/e.txt", url, NULL) == 0 && file_size("/tmp/e.txt") == 0);
    snprintf(url, sizeof(url), "http://10.0.2.2:%s/short", http_port);            /* Content-Length mai mare decat corpul */
    CHECK(run(NULL, "wget", "-q", "-O", "/tmp/s.txt", url, NULL) == 1);
    printf("nettest: [OK] faza C (wget: GET, redirectari, 404, 150000 octeti)\n");
    fflush(stdout);

    /* nc client TCP / UDP */
    put_file("/tmp/nc_in2", "hello nc\n", 9);
    CHECK(run("/tmp/nc_in2", "nc", "10.0.2.2", echo_port, NULL) == 0);
    CHECK(strcmp(out, "HELLO NC\n") == 0);
    CHECK(run(NULL, "nc", "-z", "10.0.2.2", echo_port, NULL) == 0 && has("reusita"));
    {
        static unsigned char big[100000];
        static char upper[100000];
        for(int i = 0; i < 100000; i++){
            big[i] = (unsigned char)('a' + (i * 7 + i / 26) % 26);
            upper[i] = (char)(big[i] - 32);
        }
        put_file("/tmp/nc_big", big, sizeof(big));
        CHECK(run("/tmp/nc_big", "nc", "10.0.2.2", echo_port, NULL) == 0);
        CHECK(out_len == 100000 && memcmp(out, upper, 100000) == 0);
    }
    put_file("/tmp/nc_in3", "udp hello\n", 10);
    CHECK(run("/tmp/nc_in3", "nc", "-u", "-w", "3", "10.0.2.2", echo_port, NULL) == 0);
    CHECK(strcmp(out, "UDP HELLO\n") == 0);
    printf("nettest: [OK] faza D (nc client TCP/UDP, 100000 octeti)\n");
    fflush(stdout);

    /* semnal catre gazda ca am terminat */
    put_file("/tmp/nc_done", "NETUTILS-DONE\n", 14);
    CHECK(run("/tmp/nc_done", "nc", "10.0.2.2", echo_port, NULL) == 0);
    (void)addr;
}

int main(int argc, char **argv){
    out = malloc(OUTCAP);
    if(!out || argc < 3){
        printf("nettest: FAIL argumente\n");
        return 1;
    }
    phase_a();
    phase_b(argv[1], argv[2]);
    /* tmpfs tine paginile cat exista fisierele: le stergem, ca testul kernelului sa nu vada pagini "pierdute" */
    static const char *const tmp_files[] = {"nt_out", "nc_in", "nc_out", "h.txt", "hello.txt", "index.html", "big.bin", "r1.txt",
                                            "r2.txt", "loop.txt", "e.txt", "s.txt", "nc_in2", "nc_big", "nc_in3", "nc_done"};
    for(unsigned i = 0; i < sizeof(tmp_files) / sizeof(tmp_files[0]); i++){
        char path[48];
        snprintf(path, sizeof(path), "/tmp/%s", tmp_files[i]);
        unlink(path);
    }
    free(out);
    printf("nettest: %d verificari, %d esuate\n", checks, failed);
    if(failed){
        return 1;
    }
    printf("[OK] nettest\n");
    return 0;
}

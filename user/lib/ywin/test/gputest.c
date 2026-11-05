/* gputest (T10.6): virtio-gpu 2D (/dev/gpu0) si backend-ul virtio al compozitorului. Rulat de test=gpu (QEMU cu
 * -device virtio-gpu-pci pe langa VGA-ul de boot); iese cu 0 daca totul trece.
 *   1. /dev/gpu0 direct: YGPU_IOC_INFO (geometrie, capabilitati 3D raportate), read refuzat, FLUSH fara mapare = ENXIO,
 *      mmap -> scanout activ, desen + FLUSH (unul / mai multe dreptunghiuri, decupare, n invalid), SYNC fara erori de la
 *      gazda, fork pastreaza maparea, munmap -> scanout oprit, maparea noua porneste neagra
 *   2. ycomp -b gpu si ycomp -b fb cu acelasi client (fereastra 800x520 redesenata integral la fiecare cadru): pixelii
 *      compusi, FLUSH-urile pe gpu, timpul CPU al compozitorului (/proc/<pid>/status CpuMs) pe cadru; pe gpu trebuie
 *      sa fie mai mic decat pe framebuffer
 * Cu /etc/gpuhold asteapta 3 s la "gputest: shot <nume>" (captura prin monitorul QEMU). */
#include "ywin.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ygpu.h>

#define SOCK    "/run/gputest-ycomp.sock"
#define WIN_W   800
#define WIN_H   520
#define FRAMES  150

static int checks, fails;

#define CHECK(cond, ...) do { \
    checks++; \
    if(!(cond)){ \
        fails++; \
        printf("gputest: FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while(0)

static long long ms(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void msleep(int n){
    struct timespec ts = {n / 1000, (long)(n % 1000) * 1000000L};
    while(nanosleep(&ts, &ts) < 0 && errno == EINTR){
    }
}

static void shot(const char *name){
    struct stat st;
    if(stat("/etc/gpuhold", &st) == 0){
        printf("gputest: shot %s\n", name);
        msleep(3000);
    }
}

static int info(int fd, struct yk_gpu_info *i){
    memset(i, 0, sizeof(*i));
    return ioctl(fd, YGPU_IOC_INFO, i);
}

static int flush1(int fd, int x, int y, int w, int h){
    struct yk_gpu_flush f;
    memset(&f, 0, sizeof(f));
    f.n = 1;
    f.r[0].x = (uint32_t)x;
    f.r[0].y = (uint32_t)y;
    f.r[0].w = (uint32_t)w;
    f.r[0].h = (uint32_t)h;
    return ioctl(fd, YGPU_IOC_FLUSH, &f);
}

static const char *yn(uint32_t caps, uint32_t bit){
    return (caps & bit) ? "da" : "nu";
}

/* ================= 1. /dev/gpu0 ================= */

static void test_raw(void){
    int fd = open("/dev/gpu0", O_RDWR);
    CHECK(fd >= 0, "open /dev/gpu0: errno %d", errno);
    if(fd < 0){
        return;
    }
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISCHR(st.st_mode), "/dev/gpu0 e dispozitiv de caractere");
    struct yk_gpu_info a;
    CHECK(info(fd, &a) == 0, "YGPU_IOC_INFO: errno %d", errno);
    CHECK(a.abi == YGPU_ABI && a.format == YGPU_FMT_XRGB8888, "abi %u, format %u", a.abi, a.format);
    CHECK(a.width >= 640 && a.height >= 480 && a.stride == a.width * 4, "geometrie %ux%u stride %u", a.width, a.height,
          a.stride);
    CHECK(a.map_len >= a.stride * a.height && a.map_len % 4096 == 0, "map_len %u", a.map_len);
    CHECK((a.caps & YGPU_CAP_2D) && !(a.caps & YGPU_CAP_SCANOUT), "caps 0x%x", a.caps);
    CHECK(a.num_scanouts >= 1, "scanout-uri %u", a.num_scanouts);
    CHECK(!(a.caps & YGPU_CAP_VIRGL) == !(a.features & 1), "virgl raportat din bitul VIRTIO_GPU_F_VIRGL");
    printf("gputest: virtio-gpu %ux%u, gazda %ux%u, scanout-uri %u, 3D virgl %s, blob %s, context_init %s, capset-uri %u\n",
           a.width, a.height, a.host_w, a.host_h, a.num_scanouts, yn(a.caps, YGPU_CAP_VIRGL), yn(a.caps, YGPU_CAP_BLOB),
           yn(a.caps, YGPU_CAP_CTX_INIT), a.num_capsets);
    for(uint32_t i = 0; i < a.num_capsets && i < YGPU_MAX_CAPSETS; i++){
        printf("gputest: capset %u v%u\n", a.capset[i].id, a.capset[i].max_version);
    }

    char b[16];
    errno = 0;
    CHECK(read(fd, b, sizeof(b)) < 0 && errno == EINVAL, "read refuzat: errno %d", errno);
    errno = 0;
    CHECK(flush1(fd, 0, 0, 10, 10) < 0 && errno == ENXIO, "FLUSH fara mapare: errno %d", errno);
    errno = 0;
    CHECK(ioctl(fd, 0x5999, 0) < 0 && errno == EINVAL, "ioctl necunoscut: errno %d", errno);

    uint64_t cmds0 = a.cmds;
    uint32_t *px = mmap(NULL, a.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(px != MAP_FAILED, "mmap: errno %d", errno);
    if(px == MAP_FAILED){
        close(fd);
        return;
    }
    struct yk_gpu_info i1;
    info(fd, &i1);
    CHECK(i1.caps & YGPU_CAP_SCANOUT, "scanout activ dupa mmap");
    CHECK(i1.cmds >= cmds0 + 3, "create + attach + set_scanout: %llu -> %llu", (unsigned long long)cmds0,
          (unsigned long long)i1.cmds);
    int stride = (int)(a.stride / 4);
    int zero = 1;
    for(uint32_t y = 0; y < a.height && zero; y += 37){
        for(uint32_t x = 0; x < a.width; x += 13){
            if(px[(size_t)y * stride + x]){
                zero = 0;
                break;
            }
        }
    }
    CHECK(zero, "resursa noua e neagra");

    /* Gradient + patrate */
    for(uint32_t y = 0; y < a.height; y++){
        for(uint32_t x = 0; x < a.width; x++){
            px[(size_t)y * stride + x] = ((x * 255 / a.width) << 16) | ((y * 255 / a.height) << 8) | 0x60;
        }
    }
    CHECK(flush1(fd, 0, 0, (int)a.width, (int)a.height) == 0, "FLUSH tot ecranul: errno %d", errno);
    for(int k = 0; k < 4; k++){
        for(int y = 0; y < 80; y++){
            for(int x = 0; x < 80; x++){
                px[(size_t)(100 + y) * stride + 100 + k * 120 + x] = k & 1 ? 0xFFFFFF : 0xE04040;
            }
        }
    }
    struct yk_gpu_flush f;
    memset(&f, 0, sizeof(f));
    f.n = 4;
    for(int k = 0; k < 4; k++){
        f.r[k].x = (uint32_t)(100 + k * 120);
        f.r[k].y = 100;
        f.r[k].w = 80;
        f.r[k].h = 80;
    }
    CHECK(ioctl(fd, YGPU_IOC_FLUSH, &f) == 0, "FLUSH 4 dreptunghiuri: errno %d", errno);
    CHECK(flush1(fd, (int)a.width - 10, (int)a.height - 10, 500, 500) == 0, "dreptunghi decupat la margine");
    CHECK(flush1(fd, (int)a.width + 5, 0, 10, 10) == 0, "dreptunghi in afara ecranului ignorat");
    f.n = 0;
    errno = 0;
    CHECK(ioctl(fd, YGPU_IOC_FLUSH, &f) < 0 && errno == EINVAL, "n = 0: errno %d", errno);
    f.n = YGPU_MAX_RECTS + 1;
    errno = 0;
    CHECK(ioctl(fd, YGPU_IOC_FLUSH, &f) < 0 && errno == EINVAL, "n prea mare: errno %d", errno);
    errno = 0;
    CHECK(ioctl(fd, YGPU_IOC_FLUSH, (void *)8) < 0 && errno == EFAULT, "adresa invalida: errno %d", errno);
    /* Multe flush-uri la rand: inelul se umple si se goleste (sloturile se refolosesc) */
    for(int k = 0; k < 200; k++){
        if(flush1(fd, k % 600, k % 400, 64, 64) != 0){
            CHECK(0, "flush %d: errno %d", k, errno);
            break;
        }
    }
    CHECK(ioctl(fd, YGPU_IOC_SYNC, 0) == 0, "SYNC: errno %d", errno);
    struct yk_gpu_info i2;
    info(fd, &i2);
    CHECK(i2.errors == 0, "gazda a raspuns cu %llu erori", (unsigned long long)i2.errors);
    CHECK(i2.flushes >= i1.flushes + 204, "flush-uri numarate: %llu", (unsigned long long)(i2.flushes - i1.flushes));
    CHECK(i2.cmds >= i1.cmds + 2 + 5 + 2 + 400, "comenzi: %llu", (unsigned long long)(i2.cmds - i1.cmds));
    CHECK(i2.bytes >= i1.bytes + (uint64_t)a.width * a.height * 4, "octeti transferati");
    shot("raw");

    /* fork: copilul are aceeasi mapare; la iesirea lui resursa ramane */
    pid_t p = fork();
    if(p == 0){
        int ok = px[(size_t)100 * stride + 100] == 0xE04040;
        px[(size_t)300 * stride + 300] = 0x123456;
        _exit(ok ? 0 : 1);
    }
    int stc = 0;
    waitpid(p, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "copilul vede pixelii: %d", stc);
    CHECK(px[(size_t)300 * stride + 300] == 0x123456, "scrierea copilului e in resursa (MAP_SHARED)");
    info(fd, &i2);
    CHECK(i2.caps & YGPU_CAP_SCANOUT, "scanout-ul ramane dupa iesirea copilului");

    CHECK(munmap(px, a.map_len) == 0, "munmap");
    info(fd, &i2);
    CHECK(!(i2.caps & YGPU_CAP_SCANOUT), "scanout oprit dupa ultima demapare");
    CHECK(i2.errors == 0, "fara erori la demontare: %llu", (unsigned long long)i2.errors);
    px = mmap(NULL, a.map_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(px != MAP_FAILED && px[(size_t)100 * stride + 100] == 0, "maparea noua porneste neagra");
    if(px != MAP_FAILED){
        munmap(px, a.map_len);
    }
    close(fd);
}

/* ================= 2. compozitorul pe gpu si pe fb ================= */

static long cpu_ms(pid_t pid){
    char path[48], buf[1024];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return -1;
    }
    int n = (int)read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if(n <= 0){
        return -1;
    }
    buf[n] = 0;
    char *c = strstr(buf, "CpuMs:");
    return c ? atol(c + 6) : -1;
}

typedef struct {
    long cpu;               /* ms CPU ale ycomp in fereastra de masurare */
    int frames;             /* cadre compuse in fereastra */
    long long wall;
    uint64_t flushes;       /* YGPU flush-uri in fereastra */
    int ok;
} Run;

static int stats_frames(YwConn *c){
    int32_t v[6] = {0};
    yw_query(c, YW_Q_STATS, 0, 0, v);
    return v[0];
}

static uint64_t gpu_flushes(void){
    int fd = open("/dev/gpu0", O_RDWR);
    struct yk_gpu_info i;
    uint64_t n = 0;
    if(fd >= 0 && info(fd, &i) == 0){
        n = i.flushes;
    }
    if(fd >= 0){
        close(fd);
    }
    return n;
}

static void paint(YgSurface *s, int k){
    YgColor bg = YG_RGB((uint8_t)(k * 3), (uint8_t)(90 + k % 100), (uint8_t)(200 - k % 150));
    yg_fill_rect(s, 0, 0, WIN_W, WIN_H, bg);
    int bx = (k * 7) % (WIN_W - 60);
    yg_fill_rect(s, bx, 200, 60, 60, YG_RGB(255, 255, 255));
}

static void run_comp(const char *backend, Run *r){
    memset(r, 0, sizeof(*r));
    pid_t comp = fork();
    if(comp == 0){
        char *const av[] = {"ycomp", "-s", SOCK, "-b", (char *)backend, NULL};
        execv("/bin/ycomp", av);
        _exit(127);
    }
    YwConn *c = NULL;
    for(int i = 0; i < 200 && !c; i++){
        c = yw_connect(SOCK);
        if(!c){
            msleep(50);
        }
    }
    CHECK(c != NULL, "[%s] conectare la ycomp: errno %d", backend, errno);
    if(!c){
        kill(comp, SIGKILL);
        waitpid(comp, NULL, 0);
        return;
    }
    YwWindow *w = yw_window_new(c, WIN_W, WIN_H, "gputest");
    CHECK(w != NULL, "[%s] fereastra", backend);
    if(w){
        YgSurface *s = yw_window_surface(w);
        for(int k = 0; k < 20; k++){                /* deschiderea (animatia de 200 ms) + incalzire */
            paint(s, k);
            yw_window_damage_all(w);
            yw_window_wait_frame(w, 2000);
        }
        msleep(300);
        long c0 = cpu_ms(comp);
        int f0 = stats_frames(c);
        uint64_t g0 = gpu_flushes();
        long long t0 = ms();
        int got = 0;
        for(int k = 0; k < FRAMES; k++){
            paint(s, 20 + k);
            yw_window_damage_all(w);
            got += yw_window_wait_frame(w, 2000) == 0;
        }
        r->wall = ms() - t0;
        r->cpu = cpu_ms(comp) - c0;
        r->frames = stats_frames(c) - f0;
        r->flushes = gpu_flushes() - g0;
        CHECK(got == FRAMES, "[%s] %d/%d cadre confirmate", backend, got, FRAMES);
        CHECK(c0 >= 0 && r->cpu >= 0, "[%s] CpuMs citit din /proc", backend);

        /* Pixelul compus (citit din buferul compozitorului: pe gpu chiar resursa virtio) */
        int wx, wy;
        yw_window_position(w, &wx, &wy);
        paint(s, 77);
        yw_window_damage_all(w);
        yw_window_wait_frame(w, 2000);
        msleep(50);
        int32_t v[6] = {0};
        yw_query(c, YW_Q_PIXEL, wx + 10, wy + 10, v);
        uint32_t want = YG_RGB((uint8_t)(77 * 3), (uint8_t)(90 + 77), (uint8_t)(200 - 77)) & 0xFFFFFF;
        CHECK(((uint32_t)v[0] & 0xFFFFFF) == want, "[%s] pixel compus %06x, astept %06x", backend, (unsigned)v[0], want);
        if(!strcmp(backend, "gpu")){
            shot("ycomp-gpu");
        }
        r->ok = got == FRAMES;
    }
    yw_disconnect(c);
    kill(comp, SIGTERM);
    int stc = 0;
    waitpid(comp, &stc, 0);
    CHECK(WIFEXITED(stc) && WEXITSTATUS(stc) == 0, "[%s] ycomp iese curat: %d", backend, stc);
}

static void test_compositor(void){
    Run g, f;
    run_comp("gpu", &g);
    CHECK(g.flushes >= (uint64_t)FRAMES / 2, "[gpu] compozitorul trimite FLUSH-uri: %llu", (unsigned long long)g.flushes);
    int fd = open("/dev/gpu0", O_RDWR);
    struct yk_gpu_info i;
    CHECK(fd >= 0 && info(fd, &i) == 0 && !(i.caps & YGPU_CAP_SCANOUT) && i.errors == 0,
          "dupa ycomp: scanout eliberat, fara erori");
    if(fd >= 0){
        close(fd);
    }
    run_comp("fb", &f);
    CHECK(f.flushes == 0, "[fb] fara FLUSH pe gpu: %llu", (unsigned long long)f.flushes);
    if(g.ok && f.ok && g.frames > 0 && f.frames > 0){
        long gu = g.cpu * 1000 / g.frames, fu = f.cpu * 1000 / f.frames;
        printf("gputest: CPU ycomp pe cadru: gpu %ld us (%ld ms / %d cadre, %lld ms), fb %ld us (%ld ms / %d cadre, %lld ms)\n",
               gu, g.cpu, g.frames, g.wall, fu, f.cpu, f.frames, f.wall);
        CHECK(gu < fu, "backend-ul virtio-gpu foloseste mai putin CPU decat framebuffer-ul: %ld us >= %ld us", gu, fu);
    }
}

int main(void){
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);
    test_raw();
    printf("gputest: /dev/gpu0 %d verificari, %d esecuri\n", checks, fails);
    test_compositor();
    printf("gputest: %d verificari, %d esecuri\n", checks, fails);
    if(fails == 0){
        printf("[OK] gputest\n");
    }
    return fails ? 1 : 0;
}

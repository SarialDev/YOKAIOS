/* /sbin/init: primul proces user (pornit de kernel cu stdio pe terminal).
 * Monteaza /dev /tmp /run /proc (daca nu sunt deja montate; /var, /opt si /home ca tmpfs cat timp radacina e read-only,
 * cu cate un HOME pentru fiecare utilizator din /etc/passwd cu uid >= 1000), umask 022, pune mediul de baza, porneste ysh pe consola
 * si aduna zombie-ii (wait). Daca ysh iese, il reporneste; cu `init -1` (teste) se opreste odata cu el.
 * Cu `init -l` porneste /bin/login (cere utilizator si parola) in loc de ysh (cmdline "login").
 * Cu `init -t` (modul test, test=userspace) porneste /bin/selftest in loc de ysh si iese cu codul lui.
 * Cu `init -g` (cmdline "gui") porneste sesiunea grafica, /bin/ycomp -p (compozitor + panou), in loc de ysh; se opreste cu
 * Ctrl+Alt+Backspace (init o reporneste). Fara /dev/fb0 sau daca ycomp pica imediat, cade pe ysh (T10.5). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>

#define SHELL "/bin/ysh"
#define SELFTEST "/bin/selftest"
#define LOGIN "/bin/login"
#define COMPOSITOR "/bin/ycomp"

// Linia "<sursa> <tinta> <tip> ..." din /proc/mounts cu tinta data?
static int is_mounted(const char *target){
    FILE *f = fopen("/proc/mounts", "r");
    if(!f){
        return 0;
    }
    char line[256];
    int found = 0;
    size_t n = strlen(target);
    while(!found && fgets(line, sizeof(line), f)){
        char *sp = strchr(line, ' ');
        if(sp && strncmp(sp + 1, target, n) == 0 && sp[1 + n] == ' '){
            found = 1;
        }
    }
    fclose(f);
    return found;
}

// Radacina e montata read-only (initrd)? Linia din /proc/mounts: "<sursa> / <tip> ro 0 0"
static int root_is_readonly(void){
    FILE *f = fopen("/proc/mounts", "r");
    if(!f){
        return 0;
    }
    char line[256];
    int ro = 0;
    while(fgets(line, sizeof(line), f)){
        char *sp = strchr(line, ' ');
        if(sp && sp[1] == '/' && sp[2] == ' '){
            ro = strstr(sp + 3, " ro ") != NULL;
        }
    }
    fclose(f);
    return ro;
}

static void mount_fs(const char *source, const char *target, const char *type, const char *data){
    if(is_mounted(target)){
        return;
    }
    if(mount(source, target, type, 0, data) < 0){
        printf("init: mount %s pe %s a esuat: %s\n", type, target, strerror(errno));
    }
}

// HOME-urile utilizatorilor obisnuiti (uid >= 1000, sub /home) pe tmpfs-ul nou: 0700, ai lor
static void make_homes(void){
    FILE *f = fopen("/etc/passwd", "r");
    if(!f){
        return;
    }
    char line[256];
    while(fgets(line, sizeof(line), f)){
        // nume:x:uid:gid:gecos:home:shell
        char *fl[7];
        int n = 0;
        line[strcspn(line, "\r\n")] = 0;
        fl[n++] = line;
        for(char *p = line; *p && n < 7; p++){
            if(*p == ':'){
                *p = 0;
                fl[n++] = p + 1;
            }
        }
        if(n < 6){
            continue;
        }
        unsigned uid = (unsigned)strtoul(fl[2], NULL, 10);
        unsigned gid = (unsigned)strtoul(fl[3], NULL, 10);
        const char *dir = fl[5];
        if(uid < 1000 || strncmp(dir, "/home/", 6) != 0){
            continue;
        }
        if(mkdir(dir, 0700) == 0 || errno == EEXIST){
            chown(dir, uid, gid);
        }
    }
    fclose(f);
}

static void read_hostname(void){
    char name[64];
    FILE *f = fopen("/etc/hostname", "r");
    if(f){
        if(fgets(name, sizeof(name), f)){
            name[strcspn(name, "\r\n")] = 0;
            if(*name){
                setenv("HOSTNAME", name, 1);
            }
        }
        fclose(f);
    }
}

static pid_t start_shell(int selftest, int login, int gui){
    pid_t pid = fork();
    if(pid == 0 && gui){
        const char *home = getenv("HOME");
        if(!home || chdir(home) < 0){
            chdir("/");
        }
        char *argv[] = {"ycomp", "-p", NULL};
        execv(COMPOSITOR, argv);
        printf("init: %s: %s\n", COMPOSITOR, strerror(errno));
        _exit(127);
    }
    if(pid == 0 && selftest){
        chdir("/");
        char *argv[] = {"selftest", NULL};
        execv(SELFTEST, argv);
        printf("init: %s: %s\n", SELFTEST, strerror(errno));
        _exit(127);
    }
    if(pid == 0 && login){
        chdir("/");
        char *argv[] = {"login", NULL};
        execv(LOGIN, argv);
        printf("init: %s: %s\n", LOGIN, strerror(errno));
        _exit(127);
    }
    if(pid == 0){
        const char *home = getenv("HOME");
        if(!home || chdir(home) < 0){
            chdir("/");
        }
        char *argv[] = {"ysh", "-l", NULL};
        execv(SHELL, argv);
        printf("init: %s: %s\n", SHELL, strerror(errno));
        _exit(127);
    }
    return pid;
}

int main(int argc, char **argv){
    int selftest = argc > 1 && strcmp(argv[1], "-t") == 0;
    int login = 0, gui = 0, once = selftest;
    for(int i = 1; i < argc; i++){
        if(strcmp(argv[i], "-l") == 0){
            login = 1;
        } else if(strcmp(argv[i], "-g") == 0){
            gui = 1;
        } else if(strcmp(argv[i], "-1") == 0){
            once = 1;
        }
    }

    // /proc primul: is_mounted() il citeste
    mount_fs("proc", "/proc", "proc", NULL);
    mount_fs("devfs", "/dev", "devfs", NULL);
    mount_fs("tmpfs", "/tmp", "tmpfs", "mode=1777");
    mount_fs("tmpfs", "/run", "tmpfs", "mode=0755");
    // Radacina initrd e read-only: /var (baza ypkg) si /opt (pachete .yokai) primesc tmpfs, ca sa se poata instala pachete
    // (se pierd la repornire). Cu radacina pe disc scriibil, raman cele de pe disc
    if(root_is_readonly()){
        mount_fs("tmpfs", "/var", "tmpfs", "mode=0755");
        mount_fs("tmpfs", "/opt", "tmpfs", "mode=0755");
        if(!is_mounted("/home")){
            mount_fs("tmpfs", "/home", "tmpfs", "mode=0755");
            make_homes();
        }
    }
    umask(022);

    setenv("PATH", "/bin:/sbin", 0);
    setenv("HOME", "/root", 0);
    setenv("USER", "root", 0);
    setenv("TERM", "yokai", 0);
    read_hostname();

    if(gui && access("/dev/fb0", 0) < 0){
        printf("init: fara /dev/fb0, pornesc shell-ul\n");
        gui = 0;
    }
    int status = 0;
    for(;;){
        time_t t0 = time(NULL);
        pid_t sh = start_shell(selftest, login, gui);
        if(sh < 0){
            printf("init: fork: %s\n", strerror(errno));
            sleep(1);
            continue;
        }
        // Asteapta orice copil: zombie-ii (si ceilalti) dispar; iese din bucla cand se termina shell-ul
        for(;;){
            int st;
            pid_t p = wait(&st);
            if(p == sh){
                status = st;
                break;
            }
            if(p < 0 && errno != EINTR){
                break;
            }
        }
        if(once){
            return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
        }
        if(gui && (!WIFEXITED(status) || WEXITSTATUS(status) != 0) && time(NULL) - t0 < 5){
            printf("init: sesiunea grafica a picat (status %d), pornesc shell-ul\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            gui = 0;
            continue;
        }
        printf("init: %s s-a oprit (status %d), il repornesc\n", gui ? "sesiunea grafica" : "ysh",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        struct timespec ts = {0, 200 * 1000000L};
        nanosleep(&ts, NULL);
    }
}

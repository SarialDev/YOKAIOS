/* /sbin/init: primul proces user (pornit de kernel cu stdio pe terminal).
 * Monteaza /dev /tmp /run /proc (daca nu sunt deja montate; /var si /opt ca tmpfs cat timp radacina e read-only), pune mediul de baza, porneste ysh pe consola
 * si aduna zombie-ii (wait). Daca ysh iese, il reporneste; cu `init -1` (teste) se opreste odata cu el. */
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

static pid_t start_shell(void){
    pid_t pid = fork();
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
    int once = argc > 1 && strcmp(argv[1], "-1") == 0;

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
    }

    setenv("PATH", "/bin:/sbin", 0);
    setenv("HOME", "/root", 0);
    setenv("USER", "root", 0);
    setenv("TERM", "yokai", 0);
    read_hostname();

    int status = 0;
    for(;;){
        pid_t sh = start_shell();
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
        printf("init: ysh s-a oprit (status %d), il repornesc\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        struct timespec ts = {0, 200 * 1000000L};
        nanosleep(&ts, NULL);
    }
}

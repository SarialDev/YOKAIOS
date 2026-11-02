/* Test pentru utilizatori si drepturi (test=perm): porneste ca root, pregateste fisiere in /tmp, apoi un copil devine
 * utilizatorul 1000 (yokai, grup suplimentar 100 = users) si verifica ce poate si ce nu. Iesire 0 = totul bine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "permtest: FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); } \
} while(0)

/* errno-ul unui apel care trebuie sa esueze (0 daca a reusit) */
#define ERR(call) ((call) < 0 ? errno : 0)

#define D "/tmp/pt"
#define OUTCAP 2048
static char out[OUTCAP];

static void write_file(const char *path, const char *text, mode_t mode){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if(fd >= 0){
        write(fd, text, strlen(text));
        close(fd);
    }
    chmod(path, mode);
}

static int copy_file(const char *from, const char *to, mode_t mode){
    int in = open(from, O_RDONLY);
    int o = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(in < 0 || o < 0){
        return -1;
    }
    char buf[4096];
    ssize_t n;
    while((n = read(in, buf, sizeof(buf))) > 0){
        write(o, buf, (size_t)n);
    }
    close(in);
    close(o);
    return chmod(to, mode);
}

static mode_t mode_of(const char *path){
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mode & 07777 : 0xFFFF;
}

static int owner_is(const char *path, uid_t u, gid_t g){
    struct stat st;
    return lstat(path, &st) == 0 && st.st_uid == u && st.st_gid == g;
}

/* Ruleaza path cu argv, stdout+stderr in out; codul de iesire (1000 + semnal, -1 daca nu a pornit) */
static int run(const char *path, char *const argv[]){
    int p[2];
    if(pipe(p) < 0){
        return -1;
    }
    pid_t pid = fork();
    if(pid == 0){
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        execv(path, argv);
        _exit(errno == EACCES ? 126 : 127);
    }
    close(p[1]);
    size_t len = 0;
    ssize_t n;
    while(len < OUTCAP - 1 && (n = read(p[0], out + len, OUTCAP - 1 - len)) > 0){
        len += (size_t)n;
    }
    out[len] = 0;
    close(p[0]);
    int st = 0;
    if(pid < 0 || waitpid(pid, &st, 0) != pid){
        return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
}

static int has(const char *needle){
    return strstr(out, needle) != NULL;
}

/* ---- ca root: pregatire + ce poate root ---- */

static void root_part(void){
    CHECK(getuid() == 0 && geteuid() == 0 && getgid() == 0 && getegid() == 0);
    CHECK(umask(022) == 022);                   // procesele pornite de kernel au umask 022

    // /etc/passwd, /etc/group, /etc/shadow
    struct passwd *pw = getpwnam("yokai");
    CHECK(pw && pw->pw_uid == 1000 && pw->pw_gid == 1000 && strcmp(pw->pw_dir, "/home/yokai") == 0);
    pw = getpwuid(0);
    CHECK(pw && strcmp(pw->pw_name, "root") == 0 && strcmp(pw->pw_shell, "/bin/ysh") == 0);
    struct group *gr = getgrnam("users");
    CHECK(gr && gr->gr_gid == 100 && gr->gr_mem[0] && strcmp(gr->gr_mem[0], "yokai") == 0);
    CHECK(getpwnam("nimeni") == NULL && getgrgid(4242) == NULL);
    CHECK(mode_of("/etc/shadow") == 0600 && owner_is("/etc/shadow", 0, 0));
    CHECK(mode_of("/etc/passwd") == 0644 && mode_of("/etc") == 0755 && owner_is("/etc", 0, 0));

    // Spatiul de lucru
    mkdir(D, 0755);
    CHECK(mode_of(D) == 0755 && owner_is(D, 0, 0));
    write_file(D "/rootfile", "secret\n", 0644);
    write_file(D "/zero", "z\n", 0000);
    write_file(D "/grp640", "grup\n", 0640);
    write_file(D "/grp604", "grup\n", 0604);
    CHECK(chown(D "/grp640", 0, 100) == 0 && chown(D "/grp604", 0, 100) == 0);
    CHECK(owner_is(D "/grp640", 0, 100));
    mkdir(D "/priv", 0700);
    write_file(D "/priv/f", "x\n", 0644);
    mkdir(D "/open", 0777);
    CHECK(mode_of(D "/open") == 0755);         // umask 022
    CHECK(chmod(D "/open", 0777) == 0 && mode_of(D "/open") == 0777);
    write_file(D "/open/rootf", "r\n", 0644);
    mkdir(D "/sticky", 0755);
    CHECK(chmod(D "/sticky", 01777) == 0 && mode_of(D "/sticky") == 01777);
    write_file(D "/sticky/rootf", "r\n", 0666);
    mkdir(D "/home", 0755);
    CHECK(chown(D "/home", 1000, 1000) == 0 && owner_is(D "/home", 1000, 1000));
    write_file(D "/home/sg", "", 0644);
    CHECK(chown(D "/home/sg", 1000, 0) == 0);

    // root trece de drepturi: scrie intr-un fisier 0000, citeste orice
    int fd = open(D "/zero", O_RDWR);
    CHECK(fd >= 0);
    close(fd);
    CHECK(access(D "/zero", R_OK | W_OK) == 0);
    // ... dar nu executa un fisier fara niciun bit x
    CHECK(copy_file("/bin/hello", D "/hx", 0644) == 0);
    char *hv[] = {"hello", "unu", "doi", NULL};     // hello iese cu 0 doar cu 2 argumente
    CHECK(run(D "/hx", hv) == 126);
    CHECK(access(D "/hx", X_OK) < 0 && errno == EACCES);
    CHECK(chmod(D "/hx", 0100) == 0 && run(D "/hx", hv) == 0 && has("hello from libc"));
    CHECK(chmod(D "/hx", 0711) == 0);

    // umask
    CHECK(umask(027) == 022);
    write_file(D "/um", "", 0666);
    CHECK(chmod(D "/um", 0666) == 0);           // chmod nu tine cont de umask
    int f2 = open(D "/um2", O_WRONLY | O_CREAT, 0666);
    close(f2);
    CHECK(mode_of(D "/um2") == 0640);
    mkdir(D "/umd", 0777);
    CHECK(mode_of(D "/umd") == 0750);
    CHECK(umask(022) == 027);

    // chmod/chown ca root; chown pierde setuid
    CHECK(chmod(D "/um", 04755) == 0 && mode_of(D "/um") == 04755);
    CHECK(chown(D "/um", 1001, (gid_t)-1) == 0 && owner_is(D "/um", 1001, 0) && mode_of(D "/um") == 0755);
    CHECK(chmod(D "/um", 01755) == 0 && mode_of(D "/um") == 01755);
    CHECK(lchown(D "/um", (uid_t)-1, 100) == 0 && owner_is(D "/um", 1001, 100));
    fd = open(D "/um", O_RDONLY);
    CHECK(fd >= 0 && fchmod(fd, 0600) == 0 && fchown(fd, 0, 0) == 0 && mode_of(D "/um") == 0600 && owner_is(D "/um", 0, 0));
    close(fd);
    CHECK(symlink("rootfile", D "/link") == 0 && lchown(D "/link", 1000, 1000) == 0 && owner_is(D "/link", 1000, 1000) &&
          owner_is(D "/rootfile", 0, 0));
    // Radacina initrd e read-only: chmod/chown dau EROFS chiar si pentru root
    CHECK(ERR(chmod("/etc/passwd", 0666)) == EROFS && ERR(chown("/etc/passwd", 1000, 1000)) == EROFS);
    CHECK(ERR(chmod(D "/nu_exista", 0644)) == ENOENT);
    // procfs/devfs nu au proprietari de schimbat
    CHECK(ERR(chmod("/dev/null", 0600)) == EPERM);

    // Fisiere setuid/setgid pentru copil: id ca root (4755), ca 1001 (4755), cu grupul users (2755)
    CHECK(copy_file("/bin/id", D "/id-root", 04755) == 0 && mode_of(D "/id-root") == 04755);
    CHECK(copy_file("/bin/id", D "/id-1001", 0755) == 0 && chown(D "/id-1001", 1001, 1001) == 0 &&
          chmod(D "/id-1001", 04755) == 0);
    CHECK(copy_file("/bin/id", D "/id-sgid", 0755) == 0 && chown(D "/id-sgid", 0, 100) == 0 &&
          chmod(D "/id-sgid", 02755) == 0);
    CHECK(copy_file("/bin/id", D "/id-sgid-nox", 0755) == 0 && chown(D "/id-sgid-nox", 0, 0) == 0 &&
          chmod(D "/id-sgid-nox", 02745) == 0);  // setgid fara x pe grup = blocare obligatorie, nu schimba egid
}

/* ---- ca utilizatorul 1000: rezultatul e numarul de esecuri ---- */

static void user_part(pid_t root_sleeper){
    CHECK(getuid() == 1000 && geteuid() == 1000 && getgid() == 1000 && getegid() == 1000);
    gid_t gl[8];
    CHECK(getgroups(0, NULL) == 1 && getgroups(8, gl) == 1 && gl[0] == 100);

    // Acceptare: utilizatorul 1000 nu poate scrie in /etc
    CHECK(ERR(open("/etc/nou", O_WRONLY | O_CREAT, 0644)) == EACCES);
    CHECK(ERR(open("/etc/passwd", O_WRONLY)) == EACCES);
    CHECK(ERR(open("/etc/passwd", O_RDWR | O_TRUNC)) == EACCES);
    CHECK(ERR(mkdir("/etc/d", 0755)) == EACCES);
    CHECK(ERR(unlink("/etc/hostname")) == EACCES);
    CHECK(ERR(rename("/etc/hostname", "/etc/h2")) == EACCES);
    CHECK(ERR(symlink("/tmp", "/etc/l")) == EACCES);
    CHECK(ERR(rmdir("/etc")) != 0);
    CHECK(ERR(access("/etc", W_OK)) == EACCES && access("/etc", R_OK | X_OK) == 0);
    CHECK(ERR(chmod("/etc/passwd", 0666)) == EROFS);
    // citirea merge, in afara de shadow
    int fd = open("/etc/passwd", O_RDONLY);
    CHECK(fd >= 0);
    close(fd);
    CHECK(ERR(open("/etc/shadow", O_RDONLY)) == EACCES && ERR(access("/etc/shadow", R_OK)) == EACCES);
    CHECK(getpwnam("root") != NULL);            // pwd.h citeste /etc/passwd, nu shadow
    CHECK(ERR(open("/root", O_RDONLY | O_DIRECTORY)) == EACCES && ERR(chdir("/root")) == EACCES);

    // Fara privilegii: setuid/setgroups/mount/umount
    CHECK(ERR(setuid(0)) == EPERM && ERR(setgid(0)) == EPERM && geteuid() == 1000);
    CHECK(setuid(1000) == 0 && setgid(1000) == 0);
    gid_t z = 0;
    CHECK(ERR(setgroups(1, &z)) == EPERM);
    CHECK(ERR(mount("none", D "/home", "tmpfs", 0, NULL)) == EPERM && ERR(umount("/tmp")) == EPERM);

    // Fisierele lui root
    CHECK(ERR(open(D "/rootfile", O_WRONLY)) == EACCES);
    fd = open(D "/rootfile", O_RDONLY);
    CHECK(fd >= 0);
    close(fd);
    CHECK(ERR(open(D "/zero", O_RDONLY)) == EACCES);
    CHECK(ERR(chmod(D "/rootfile", 0666)) == EPERM && ERR(chown(D "/rootfile", 1000, 1000)) == EPERM);
    CHECK(ERR(unlink(D "/rootfile")) == EACCES);    // D e 0755 al lui root
    CHECK(ERR(open(D "/nou", O_WRONLY | O_CREAT, 0644)) == EACCES);
    // grup: 0640 root:users se citeste (1000 e in users), 0604 nu: clasa "grup" se potriveste inaintea lui "other"
    fd = open(D "/grp640", O_RDONLY);
    CHECK(fd >= 0);
    close(fd);
    CHECK(ERR(open(D "/grp640", O_WRONLY)) == EACCES);
    CHECK(ERR(open(D "/grp604", O_RDONLY)) == EACCES);
    // Director 0700 al lui root: nici cautare, nici listare
    struct stat st;
    CHECK(ERR(stat(D "/priv/f", &st)) == EACCES && ERR(chdir(D "/priv")) == EACCES);
    CHECK(ERR(open(D "/priv", O_RDONLY | O_DIRECTORY)) == EACCES);
    CHECK(stat(D "/priv", &st) == 0);           // directorul insusi se vede

    // Fisiere noi: proprietar = euid/egid, mode cu umask
    fd = open(D "/home/f", O_WRONLY | O_CREAT, 0666);
    CHECK(fd >= 0 && write(fd, "abc", 3) == 3);
    close(fd);
    CHECK(owner_is(D "/home/f", 1000, 1000) && mode_of(D "/home/f") == 0644);
    CHECK(mkdir(D "/home/d", 0777) == 0 && owner_is(D "/home/d", 1000, 1000) && mode_of(D "/home/d") == 0755);
    CHECK(symlink("f", D "/home/l") == 0 && owner_is(D "/home/l", 1000, 1000));
    // chmod/chown pe ale lui: grupul doar spre unul din grupurile lui, uid-ul niciodata
    CHECK(chmod(D "/home/f", 0600) == 0 && mode_of(D "/home/f") == 0600);
    CHECK(chown(D "/home/f", (uid_t)-1, 100) == 0 && owner_is(D "/home/f", 1000, 100));
    CHECK(ERR(chown(D "/home/f", (uid_t)-1, 0)) == EPERM && ERR(chown(D "/home/f", 0, (gid_t)-1)) == EPERM);
    CHECK(chown(D "/home/f", 1000, 1000) == 0);
    // proprietarul isi poate lua singur dreptul de scriere (si root nu e)
    CHECK(chmod(D "/home/f", 0400) == 0 && ERR(open(D "/home/f", O_WRONLY)) == EACCES);
    CHECK(access(D "/home/f", R_OK) == 0 && ERR(access(D "/home/f", W_OK)) == EACCES);
    // setgid: pe fisierul lui cu grupul lui ramane; pe unul cu un grup in care nu e (root i-a dat grupul 0), bitul cade
    CHECK(chmod(D "/home/f", 02644) == 0 && mode_of(D "/home/f") == 02644);
    CHECK(chmod(D "/home/sg", 02644) == 0 && mode_of(D "/home/sg") == 0644);
    // Director cu setgid: fisierele noi iau grupul directorului, subdirectoarele si bitul
    CHECK(chown(D "/home/d", (uid_t)-1, 100) == 0 && chmod(D "/home/d", 02775) == 0);
    fd = open(D "/home/d/g", O_WRONLY | O_CREAT, 0644);
    close(fd);
    CHECK(owner_is(D "/home/d/g", 1000, 100));
    CHECK(mkdir(D "/home/d/sub", 0755) == 0 && owner_is(D "/home/d/sub", 1000, 100) &&
          (mode_of(D "/home/d/sub") & S_ISGID));

    // Director 0777 fara sticky: oricine sterge orice; cu sticky (1777) doar ale lui
    CHECK(unlink(D "/open/rootf") == 0);
    CHECK(ERR(unlink(D "/sticky/rootf")) == EPERM && ERR(rename(D "/sticky/rootf", D "/sticky/x")) == EPERM);
    fd = open(D "/sticky/rootf", O_WRONLY);     // 0666: scrierea in el merge, doar stergerea nu
    CHECK(fd >= 0);
    close(fd);
    fd = open(D "/sticky/mine", O_WRONLY | O_CREAT, 0644);
    close(fd);
    CHECK(rename(D "/sticky/mine", D "/sticky/mine2") == 0 && unlink(D "/sticky/mine2") == 0);
    // /tmp e 1777
    CHECK(mode_of("/tmp") == 01777);

    // exec: fara r merge (0711), fara x nu
    char *hv[] = {"hello", "unu", "doi", NULL};     // hello iese cu 0 doar cu 2 argumente
    CHECK(run(D "/hx", hv) == 0 && has("hello from libc"));
    CHECK(ERR(open(D "/hx", O_RDONLY)) == EACCES);
    CHECK(chmod(D "/home/f", 0644) == 0 && run(D "/home/f", hv) == 126);
    // setuid/setgid la exec
    char *iv[] = {"id", NULL};
    CHECK(run("/bin/id", iv) == 0 && strcmp(out, "uid=1000(yokai) gid=1000(yokai) groups=1000(yokai),100(users)\n") == 0);
    CHECK(run(D "/id-root", iv) == 0 && has("uid=1000(yokai) gid=1000(yokai) euid=0(root) groups="));
    CHECK(run(D "/id-1001", iv) == 0 && has(" euid=1001 "));
    CHECK(run(D "/id-sgid", iv) == 0 && has(" egid=100(users) ") && !has("euid="));
    CHECK(run(D "/id-sgid-nox", iv) == 0 && !has("egid="));
    char *iu[] = {"id", "-un", NULL};
    CHECK(run("/bin/id", iu) == 0 && strcmp(out, "yokai\n") == 0);
    char *wv[] = {"whoami", NULL};
    CHECK(run("/bin/whoami", wv) == 0 && strcmp(out, "yokai\n") == 0);
    // /proc/self/status
    FILE *f = fopen("/proc/self/status", "r");
    char line[128];
    int uid_ok = 0, gid_ok = 0;
    while(f && fgets(line, sizeof(line), f)){
        uid_ok |= strcmp(line, "Uid:\t1000\t1000\t1000\t1000\n") == 0;
        gid_ok |= strcmp(line, "Gid:\t1000\t1000\t1000\t1000\n") == 0;
    }
    if(f){
        fclose(f);
    }
    CHECK(uid_ok && gid_ok);
    // su fara root
    char *sv[] = {"su", "-c", "id", NULL};
    CHECK(run("/bin/su", sv) == 1 && has("doar root"));
    // utilitarele: chmod/chown/ls -l
    char *cm[] = {"chmod", "u+x,g-r,o=", D "/home/f", NULL};
    CHECK(run("/bin/chmod", cm) == 0 && mode_of(D "/home/f") == 0700);
    char *cm2[] = {"chmod", "640", D "/home/f", NULL};
    CHECK(run("/bin/chmod", cm2) == 0 && mode_of(D "/home/f") == 0640);
    char *cm3[] = {"chmod", "644", D "/rootfile", NULL};
    CHECK(run("/bin/chmod", cm3) == 1 && has("Operation not permitted"));
    char *co[] = {"chown", ":users", D "/home/f", NULL};
    CHECK(run("/bin/chown", co) == 0 && owner_is(D "/home/f", 1000, 100));
    char *co2[] = {"chown", "root", D "/home/f", NULL};
    CHECK(run("/bin/chown", co2) == 1);
    char *lv[] = {"ls", "-l", D "/home", NULL};
    CHECK(run("/bin/ls", lv) == 0 && has("-rw-r----- yokai    users           3 f\n") && has("drwxrwsr-x yokai    users"));

    // Semnale: nu la procesele lui root, da la ale lui
    CHECK(ERR(kill(root_sleeper, SIGTERM)) == EPERM);
    pid_t mine = fork();
    if(mine == 0){
        for(;;){
            pause();
        }
    }
    CHECK(mine > 0 && kill(mine, SIGTERM) == 0);
    int st2;
    CHECK(waitpid(mine, &st2, 0) == mine && WIFSIGNALED(st2) && WTERMSIG(st2) == SIGTERM);
}

int main(void){
    root_part();

    pid_t sleeper = fork();
    if(sleeper == 0){
        for(;;){
            pause();
        }
    }

    pid_t child = fork();
    if(child == 0){
        gid_t users = 100;
        if(setgroups(1, &users) < 0 || setgid(1000) < 0 || setuid(1000) < 0){
            fprintf(stderr, "permtest: nu pot deveni 1000: %s\n", strerror(errno));
            _exit(100);
        }
        user_part(sleeper);
        printf("permtest: utilizator 1000: %d verificari, %d esuate\n", checks, failed);
        _exit(failed ? 1 : 0);
    }
    int st = 0;
    CHECK(child > 0 && waitpid(child, &st, 0) == child && WIFEXITED(st) && WEXITSTATUS(st) == 0);

    // root poate omori orice; su catre yokai ruleaza comanda ca 1000
    CHECK(kill(sleeper, SIGKILL) == 0 && waitpid(sleeper, &st, 0) == sleeper);
    char *sv[] = {"su", "yokai", "-c", "id -un; touch /etc/x", NULL};
    CHECK(run("/bin/su", sv) == 1 && has("yokai\n") && has("touch: /etc/x: Permission denied"));
    char *sv2[] = {"su", "-", "yokai", "-c", "pwd", NULL};
    CHECK(run("/bin/su", sv2) == 0 && strcmp(out, "/home/yokai\n") == 0);

    // Curatenie: paginile tmpfs se intorc (kernelul verifica la final ca nu s-a pierdut memorie)
    char *rm[] = {"rm", "-rf", D, NULL};
    CHECK(run("/bin/rm", rm) == 0 && ERR(access(D, F_OK)) == ENOENT);

    printf("permtest: %d verificari (root), %d esuate\n", checks, failed);
    if(!failed){
        printf("[OK] permtest\n");
    }
    return failed ? 1 : 0;
}

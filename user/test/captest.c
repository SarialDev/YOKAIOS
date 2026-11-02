/* Test pentru capabilitati per proces (test=caps): ruleaza ca root cu CAP_ALL si verifica mostenirea la fork, reducerea
 * (capset, trecerea de la root la utilizator, politica de la exec) si verificarea in syscall-urile sensibile: mount,
 * sched_setscheduler (RT), ioperm, kill, setuid. Iesire 0 = totul bine */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sched.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/io.h>
#include <sys/capability.h>

static int checks, failed;

#define CHECK(c) do { \
    checks++; \
    if(!(c)){ failed++; fprintf(stderr, "captest: FAIL %s:%d: %s (errno %d)\n", __FILE__, __LINE__, #c, errno); } \
} while(0)

#define ERR(call) ((call) < 0 ? errno : 0)
#define D "/tmp/ct"

static unsigned long long eff(void){
    struct cap_state st;
    return capget(0, &st) == 0 ? st.effective : ~0ULL;
}

static unsigned long long perm(void){
    struct cap_state st;
    return capget(0, &st) == 0 ? st.permitted : ~0ULL;
}

/* Reduce procesul la exact `keep` (effective = permitted = keep) */
static int only(unsigned long long keep){
    struct cap_state st = {keep, keep};
    return capset(&st);
}

/* Ruleaza fn intr-un copil; codul de iesire (1000 + semnal daca a murit de semnal) */
static int child(void (*fn)(void)){
    pid_t pid = fork();
    if(pid == 0){
        fn();
        _exit(0);
    }
    int st = 0;
    if(pid < 0 || waitpid(pid, &st, 0) != pid){
        return -1;
    }
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1000 + WTERMSIG(st);
}

static void write_file(const char *path, const char *text){
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if(fd >= 0){
        write(fd, text, strlen(text));
        close(fd);
    }
}

/* Ruleaza programul cu argumente, stdout in out */
static char out[512];
static int run(char *const argv[]){
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
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    size_t len = 0;
    ssize_t n;
    while(len < sizeof(out) - 1 && (n = read(p[0], out + len, sizeof(out) - 1 - len)) > 0){
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

/* ---- copii ---- */

static void c_inherit(void){
    _exit(eff() == CAP_ALL && perm() == CAP_ALL ? 0 : 1);
}

static void c_drop_rt(void){
    struct cap_state st = {CAP_ALL & ~CAP_RT, CAP_ALL & ~CAP_RT};
    if(capset(&st) < 0 || eff() != (CAP_ALL & ~CAP_RT)){
        _exit(1);
    }
    // restul parintelui nu se schimba (verificat dupa); copilul nu poate recapata CAP_RT
    st.effective = st.permitted = CAP_ALL;
    _exit(capset(&st) < 0 && errno == EPERM ? 0 : 2);
}

static void c_capset_rules(void){
    struct cap_state st = {CAP_RT, CAP_RT | CAP_NET};
    if(capset(&st) < 0){                         // effective in permitted: ok
        _exit(1);
    }
    st.effective = CAP_KILL;                     // effective in afara lui permitted
    if(capset(&st) == 0 || errno != EPERM){
        _exit(2);
    }
    st.permitted = CAP_RT | CAP_NET | CAP_KILL;  // permitted ar creste
    st.effective = CAP_RT;
    if(capset(&st) == 0 || errno != EPERM){
        _exit(3);
    }
    st.permitted = 1ULL << 40;                   // biti necunoscuti
    st.effective = 0;
    if(capset(&st) == 0 || errno != EINVAL){
        _exit(4);
    }
    if(capget(99999, &st) == 0 || errno != ESRCH){
        _exit(5);
    }
    struct cap_state parent;
    if(capget(getppid(), &parent) != 0 || parent.effective != CAP_ALL){
        _exit(6);
    }
    _exit(0);
}

static void c_rt_ok(void){
    struct sched_param sp = {10};
    if(sched_getscheduler(0) != SCHED_OTHER){
        _exit(1);
    }
    if(sched_setscheduler(0, SCHED_FIFO, &sp) < 0 || sched_getscheduler(0) != SCHED_FIFO){
        _exit(2);
    }
    // un copil al unui proces RT porneste NORMAL
    pid_t k = fork();
    if(k == 0){
        _exit(sched_getscheduler(0) == SCHED_OTHER ? 0 : 1);
    }
    int st = 0;
    if(waitpid(k, &st, 0) != k || !WIFEXITED(st) || WEXITSTATUS(st) != 0){
        _exit(3);
    }
    sp.sched_priority = 0;
    if(sched_setscheduler(0, SCHED_FIFO, &sp) == 0 || errno != EINVAL){
        _exit(4);
    }
    sp.sched_priority = 100;
    if(sched_setscheduler(0, SCHED_FIFO, &sp) == 0 || errno != EINVAL){
        _exit(5);
    }
    sp.sched_priority = 0;
    if(sched_setscheduler(0, SCHED_OTHER, &sp) < 0 || sched_getscheduler(0) != SCHED_OTHER){
        _exit(6);
    }
    _exit(0);
}

// Ca root (uid 0), dar fara CAP_RT: nu poate crea task-uri RT
static void c_rt_denied(void){
    struct sched_param sp = {10};
    if(only(CAP_ALL & ~CAP_RT) < 0 || getuid() != 0 || geteuid() != 0){
        _exit(1);
    }
    if(sched_setscheduler(0, SCHED_FIFO, &sp) == 0 || errno != EPERM){
        _exit(2);
    }
    if(sched_getscheduler(0) != SCHED_OTHER){
        _exit(3);
    }
    sp.sched_priority = 0;
    if(sched_setscheduler(0, SCHED_OTHER, &sp) < 0){      // revenirea la NORMAL nu cere nimic
        _exit(4);
    }
    // nici dupa fork / exec nu reapare
    _exit(0);
}

static void c_mount_denied(void){
    if(only(CAP_ALL & ~(CAP_MOUNT | CAP_SYS_ADMIN)) < 0){
        _exit(1);
    }
    if(mount("none", D "/m", "tmpfs", 0, NULL) == 0 || errno != EPERM){
        _exit(2);
    }
    if(umount(D "/m") == 0 || errno != EPERM){
        _exit(3);
    }
    _exit(0);
}

static void c_mount_cap(void){
    if(only(CAP_MOUNT) < 0 || mount("none", D "/m", "tmpfs", 0, NULL) != 0){
        _exit(1);
    }
    if(umount(D "/m") != 0){
        _exit(2);
    }
    _exit(0);
}

static void c_mount_sysadmin(void){
    if(only(CAP_SYS_ADMIN) < 0 || mount("none", D "/m", "tmpfs", 0, NULL) != 0 || umount(D "/m") != 0){
        _exit(1);
    }
    _exit(0);
}

static pid_t sleeper_pid;
static int sync_wr;

// Proces cu alt uid, pe care root il poate omori doar cu CAP_KILL
static void c_user_sleeper(void){
    if(setgid(1000) < 0 || setuid(1000) < 0){
        _exit(1);
    }
    write(sync_wr, "x", 1);
    for(;;){
        pause();
    }
}

static void c_kill_denied(void){
    if(only(CAP_ALL & ~CAP_KILL) < 0){
        _exit(1);
    }
    if(kill(sleeper_pid, SIGTERM) == 0 || errno != EPERM){
        _exit(2);
    }
    _exit(0);
}

static void c_kill_ok(void){
    if(only(CAP_KILL) < 0 || kill(sleeper_pid, 0) != 0){
        _exit(1);
    }
    _exit(0);
}

static void c_setuid_denied(void){
    if(only(CAP_ALL & ~CAP_SETUID) < 0){
        _exit(1);
    }
    if(setuid(1000) == 0 || errno != EPERM || setgid(1000) == 0 || errno != EPERM){
        _exit(2);
    }
    gid_t g = 5;
    if(setgroups(1, &g) == 0 || errno != EPERM){
        _exit(3);
    }
    if(getuid() != 0 || perm() != (CAP_ALL & ~CAP_SETUID)){
        _exit(4);
    }
    _exit(0);
}

// Dupa trecerea la un utilizator (toate id-urile nenule) capabilitatile dispar
static void c_setuid_drops(void){
    if(setgid(1000) < 0 || setuid(1000) < 0){
        _exit(1);
    }
    if(eff() != 0 || perm() != 0){
        _exit(2);
    }
    struct sched_param sp = {10};
    if(sched_setscheduler(0, SCHED_FIFO, &sp) == 0 || errno != EPERM){
        _exit(3);
    }
    _exit(0);
}

// Doar euid se schimba: effective dispare, permitted ramane, iar revenirea la 0 il reface
static void c_seteuid_cycle(void){
    if(setreuid((uid_t)-1, 1000) < 0 || eff() != 0 || perm() != CAP_ALL){
        _exit(1);
    }
    if(setreuid((uid_t)-1, 0) < 0 || eff() != CAP_ALL){
        _exit(2);
    }
    _exit(0);
}

static void c_ioperm_denied(void){
    if(only(CAP_ALL & ~CAP_RAWIO) < 0){
        _exit(1);
    }
    if(ioperm(0x80, 1, 1) == 0 || errno != EPERM){
        _exit(2);
    }
    _exit(0);
}

static void c_ioperm_args(void){
    if(ioperm(0xFFFF, 2, 1) == 0 || errno != EINVAL || ioperm(0x10000, 1, 1) == 0 || errno != EINVAL){
        _exit(1);
    }
    _exit(0);
}

static void c_io_noperm(void){
    outb(0, 0x80);                  // fara ioperm: #GP -> SIGSEGV
    _exit(0);
}

static void c_io_ok(void){
    if(ioperm(0x80, 1, 1) != 0){
        _exit(1);
    }
    outb(0x5A, 0x80);
    (void)inb(0x80);
    if(ioperm(0x80, 1, 0) != 0){
        _exit(2);
    }
    _exit(0);
}

static void c_io_revoked(void){
    if(ioperm(0x80, 1, 1) != 0){
        _exit(1);
    }
    outb(1, 0x80);
    if(ioperm(0x80, 1, 0) != 0){
        _exit(2);
    }
    outb(2, 0x80);                  // revocat: SIGSEGV
    _exit(0);
}

// ioperm nu se mosteneste la fork
static void c_io_fork(void){
    if(ioperm(0x80, 1, 1) != 0){
        _exit(1);
    }
    pid_t k = fork();
    if(k == 0){
        c_io_noperm();
    }
    int st = 0;
    if(waitpid(k, &st, 0) != k || !WIFSIGNALED(st)){
        _exit(2);
    }
    outb(3, 0x80);                  // parintele pastreaza accesul
    _exit(0);
}

static void c_port_check(void){
    // doar procesul care are permisiunea ruleaza pe un alt CPU / dupa o comutare: mai multe schimbari de context
    if(ioperm(0x80, 1, 1) != 0){
        _exit(1);
    }
    for(int i = 0; i < 50; i++){
        outb((unsigned char)i, 0x80);
        sched_yield();
    }
    _exit(0);
}

int main(void){
    struct cap_state st;
    CHECK(capget(0, &st) == 0 && st.effective == CAP_ALL && st.permitted == CAP_ALL);
    CHECK(capget(getpid(), &st) == 0 && st.effective == CAP_ALL);
    CHECK(capget(1, &st) == 0 || errno == ESRCH);
    CHECK(capget(0, NULL) < 0 && errno == EFAULT);
    CHECK(getuid() == 0);

    mkdir(D, 0755);
    mkdir(D "/m", 0755);

    // mostenire la fork, reducere in copil, nimic nu creste
    CHECK(child(c_inherit) == 0);
    CHECK(child(c_drop_rt) == 0);
    CHECK(eff() == CAP_ALL);
    CHECK(child(c_capset_rules) == 0);

    // RT: cu CAP_RT da, fara CAP_RT nu (acceptarea sarcinii)
    CHECK(child(c_rt_ok) == 0);
    CHECK(child(c_rt_denied) == 0);
    CHECK(sched_getscheduler(0) == SCHED_OTHER);

    // mount / umount
    CHECK(child(c_mount_denied) == 0);
    CHECK(child(c_mount_cap) == 0);
    CHECK(child(c_mount_sysadmin) == 0);

    // kill catre alt utilizator
    int sp[2];
    CHECK(pipe(sp) == 0);
    sync_wr = sp[1];
    sleeper_pid = fork();
    if(sleeper_pid == 0){
        close(sp[0]);
        c_user_sleeper();
    }
    close(sp[1]);
    char go;
    CHECK(read(sp[0], &go, 1) == 1);             // sleeper-ul a devenit 1000
    close(sp[0]);
    CHECK(child(c_kill_denied) == 0);
    CHECK(child(c_kill_ok) == 0);
    CHECK(kill(sleeper_pid, SIGKILL) == 0);      // root cu CAP_KILL
    int s = 0;
    CHECK(waitpid(sleeper_pid, &s, 0) == sleeper_pid && WIFSIGNALED(s));

    // setuid / identitate
    CHECK(child(c_setuid_denied) == 0);
    CHECK(child(c_setuid_drops) == 0);
    CHECK(child(c_seteuid_cycle) == 0);

    // ioperm
    CHECK(child(c_ioperm_denied) == 0);
    CHECK(child(c_ioperm_args) == 0);
    CHECK(child(c_io_noperm) == 1000 + SIGSEGV);
    CHECK(child(c_io_ok) == 0);
    CHECK(child(c_io_revoked) == 1000 + SIGSEGV);
    CHECK(child(c_io_fork) == 0);
    CHECK(child(c_port_check) == 0);

    // exec: politica din /var/lib/ypkg/exec.caps reduce plafonul (si prin symlink), fara politica se mosteneste
    char *capsv[] = {"/bin/caps", NULL};
    CHECK(run(capsv) == 0 && strcmp(out, "permitted: sys_admin,net,rawio,rt,mount,kill,setuid\neffective: sys_admin,net,rawio,rt,mount,kill,setuid\n") == 0);
    char *dropv[] = {"/bin/caps", "-d", "rt,kill", "/bin/caps", NULL};
    CHECK(run(dropv) == 0 && strcmp(out, "permitted: sys_admin,net,rawio,mount,setuid\neffective: sys_admin,net,rawio,mount,setuid\n") == 0);
    char *bad[] = {"/bin/caps", "-d", "nimic", NULL};
    CHECK(run(bad) == 2);
    CHECK(mount("none", "/var", "tmpfs", 0, NULL) == 0);       // /var din initrd e read-only (fara init)
    mkdir("/var/lib", 0755);
    mkdir("/var/lib/ypkg", 0755);
    CHECK(symlink("/bin/caps", D "/lnk") == 0);
    write_file("/var/lib/ypkg/exec.caps", "# test\n/bin/caps rt,net captest\n/bin/echo none captest\n");
    CHECK(run(capsv) == 0 && strcmp(out, "permitted: net,rt\neffective: net,rt\n") == 0);
    char *lnkv[] = {D "/lnk", NULL};
    CHECK(run(lnkv) == 0 && strcmp(out, "permitted: net,rt\neffective: net,rt\n") == 0);
    // plafonul se aplica peste ce avea deja procesul; nu adauga nimic
    CHECK(run(dropv) == 0 && strcmp(out, "permitted: net\neffective: net\n") == 0);
    char *dropall[] = {"/bin/caps", "-d", "rt,net", "/bin/caps", NULL};
    CHECK(run(dropall) == 0 && strcmp(out, "permitted: none\neffective: none\n") == 0);
    write_file("/var/lib/ypkg/exec.caps", "/bin/caps rt captest\n/bin/caps all captest\n");   // ultima linie castiga
    CHECK(run(capsv) == 0 && strncmp(out, "permitted: sys_admin", 20) == 0);
    write_file("/var/lib/ypkg/exec.caps", "/bin/caps rt,inexistent captest\n");              // linie invalida se ignora
    CHECK(run(capsv) == 0 && strncmp(out, "permitted: sys_admin", 20) == 0);
    unlink("/var/lib/ypkg/exec.caps");
    CHECK(run(capsv) == 0 && strncmp(out, "permitted: sys_admin", 20) == 0);

    // curatenie
    CHECK(umount("/var") == 0);
    unlink(D "/lnk");
    rmdir(D "/m");
    rmdir(D);

    printf("captest: %d verificari, %d esuate\n", checks, failed);
    if(!failed){
        printf("[OK] captest\n");
    }
    return failed ? 1 : 0;
}

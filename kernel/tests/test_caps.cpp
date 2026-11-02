#include "tests/test_caps.h"
#include "proc/caps.h"
#include "proc/cred.h"
#include "proc/process.h"
#include "exec/elf.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "lib/string.h"

namespace {

const char* const envp[] = {"YOKAI=1", "PATH=/bin:/sbin", nullptr};

int fails = 0;

void check(bool ok, const char* what){
    if(!ok){
        fails++;
        kprintf("test_caps: FAIL %s\n", what);
    }
}

#define CHECK(c) check((c), #c)

Cred cred_of(uint32_t uid, uint32_t euid, uint32_t suid){
    Cred c = {};
    c.uid = uid;
    c.euid = euid;
    c.suid = suid;
    return c;
}

// Functiile pure: verificari, porturi privilegiate, trecerea de identitate, parsare / formatare
void pure_checks(){
    const Caps all = caps_full;
    const Caps none = {0, 0};
    CHECK(caps_has(all, CAP_RT) && caps_has(all, CAP_KILL) && !caps_has(none, CAP_RT));
    // CAP_SYS_ADMIN include CAP_MOUNT, dar nu si restul
    Caps admin = {CAP_SYS_ADMIN, CAP_SYS_ADMIN};
    CHECK(caps_has(admin, CAP_MOUNT) && !caps_has(admin, CAP_RT) && !caps_has(admin, CAP_NET) && !caps_has(admin, CAP_RAWIO));
    // permitted fara effective nu conteaza la verificare
    Caps latent = {CAP_RT, 0};
    CHECK(!caps_has(latent, CAP_RT));

    // porturi < 1024 cer CAP_NET
    Caps net = {CAP_NET, CAP_NET};
    CHECK(caps_may_bind(none, 1024) && caps_may_bind(none, 8080) && caps_may_bind(none, 65535));
    CHECK(!caps_may_bind(none, 80) && !caps_may_bind(none, 1023) && !caps_may_bind(none, 0));
    CHECK(caps_may_bind(net, 80) && caps_may_bind(net, 22) && caps_may_bind(all, 443));
    CHECK(!caps_may_bind(admin, 80));

    // trecerea de identitate
    Caps c = all;
    caps_on_setid(c, cred_of(0, 0, 0), cred_of(1000, 1000, 1000));
    CHECK(c.permitted == 0 && c.effective == 0);
    c = all;
    caps_on_setid(c, cred_of(0, 0, 0), cred_of(0, 1000, 0));              // doar euid: effective se pierde
    CHECK(c.permitted == CAP_ALL && c.effective == 0);
    caps_on_setid(c, cred_of(0, 1000, 0), cred_of(0, 0, 0));              // inapoi: effective se reface
    CHECK(c.permitted == CAP_ALL && c.effective == CAP_ALL);
    c = all;
    caps_on_setid(c, cred_of(0, 0, 0), cred_of(1000, 1000, 0));           // suid ramane 0: permitted pastrat
    CHECK(c.permitted == CAP_ALL && c.effective == 0);
    c = Caps{0, 0};
    caps_on_setid(c, cred_of(1000, 1000, 1000), cred_of(1000, 0, 1000));  // setuid-root exec fara capabilitati: nu apar
    CHECK(c.permitted == 0 && c.effective == 0);

    // plafon
    c = all;
    caps_limit(c, CAP_RT | CAP_NET);
    CHECK(c.permitted == (CAP_RT | CAP_NET) && c.effective == (CAP_RT | CAP_NET));
    c = Caps{CAP_RT, CAP_RT};
    caps_limit(c, CAP_NET);
    CHECK(c.permitted == 0 && c.effective == 0);

    // liste de nume
    uint64_t m;
    CHECK(caps_parse("rt,net", m) && m == (CAP_RT | CAP_NET));
    CHECK(caps_parse("none", m) && m == 0);
    CHECK(caps_parse("all", m) && m == CAP_ALL);
    CHECK(caps_parse("0x28", m) && m == (CAP_RT | CAP_KILL));
    CHECK(caps_parse("mount,kill,setuid,sys_admin,rawio", m) && m == (CAP_MOUNT | CAP_KILL | CAP_SETUID | CAP_SYS_ADMIN | CAP_RAWIO));
    CHECK(!caps_parse("rt,nimic", m) && !caps_parse("0xzz", m));
    char buf[96];
    caps_format(0, buf, sizeof(buf));
    CHECK(memcmp(buf, "none", 5) == 0);
    caps_format(CAP_RT | CAP_NET, buf, sizeof(buf));
    CHECK(memcmp(buf, "net,rt", 7) == 0);
    caps_format(CAP_ALL, buf, sizeof(buf));
    CHECK(memcmp(buf, "sys_admin,net,rawio,rt,mount,kill,setuid", 41) == 0);
    caps_format(CAP_ALL, buf, 12);                                        // buffer mic: se trunchiaza fara depasire
    CHECK(memcmp(buf, "sys_admin", 10) == 0);

    // thread-urile de kernel (fara proces) au tot
    CHECK(caps_current().effective == CAP_ALL && capable(CAP_RT) && capable(CAP_MOUNT));
}

// Porneste programul si asteapta sa fie eliberat; codul de iesire
int run(const char* path, const char* const* argv){
    uint64_t tasks_before = task_count();
    uint64_t pid = exec_spawn(path, argv, envp);
    if(!pid){
        panic("test_caps: %s nu a pornit", path);
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_caps: %s (pid %lu) a disparut fara exit", path, pid);
    }
    return code;
}

}

void test_caps_run(){
    __asm__ volatile("sti");
    pure_checks();
    if(fails){
        panic("test_caps: %d verificari esuate", fails);
    }
    static const char* const argv[] = {"captest", nullptr};
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    int code = run("/bin/captest", argv);
    if(code != 0){
        panic("test_caps: captest a iesit cu %d (vezi liniile \"captest: FAIL\")", code);
    }
    if(process_count() != procs_before){
        panic("test_caps: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 8 < free_before){
        panic("test_caps: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_caps (fork, capset, mount, RT, ioperm, kill, setuid, exec)\n");
}

#include "tests/test_wm.h"
#include "drivers/fb/fb.h"
#include "drivers/yinput.h"
#include "exec/elf.h"
#include "proc/process.h"
#include "sched/sched.h"
#include "mm/pmm.h"
#include "ipc/shm.h"
#include "net/unix.h"
#include "core/panic.h"
#include "lib/kprintf.h"

namespace {

const char* const envp[] = {"YOKAI=1", "PATH=/bin:/sbin", nullptr};

}

void test_wm_run(){
    __asm__ volatile("sti");
    if(!fb_ready()){
        panic("test_wm: nu exista framebuffer (cmdline nofb sau GRUB fara mod grafic)");
    }
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    uint64_t tasks_before = task_count();

    static const char* const argv[] = {"wmtest", nullptr};
    uint64_t pid = exec_spawn("/bin/wmtest", argv, envp);
    if(!pid){
        panic("test_wm: /bin/wmtest nu a pornit");
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_wm: wmtest (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_wm: wmtest a iesit cu %d (vezi liniile \"wmtest: FAIL\")", code);
    }
    if(fb_user_owned()){
        panic("test_wm: dupa ycomp ecranul a ramas in modul grafic");
    }
    if(yinput_grabbed()){
        panic("test_wm: captura de input a ramas");
    }
    if(shm_count() != 0){
        panic("test_wm: %lu obiecte shm ramase", shm_count());
    }
    if(unix_count() != 0){
        panic("test_wm: %lu socket-uri AF_UNIX ramase", unix_count());
    }
    if(process_count() != procs_before){
        panic("test_wm: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 16 < free_before){
        panic("test_wm: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_wm (ycomp, managerul de ferestre)\n");
}

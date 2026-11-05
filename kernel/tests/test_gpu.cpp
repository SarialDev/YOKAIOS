#include "tests/test_gpu.h"
#include "drivers/fb/fb.h"
#include "drivers/gpu/virtio_gpu.h"
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

void test_gpu_run(){
    __asm__ volatile("sti");
    if(!fb_ready()){
        panic("test_gpu: nu exista framebuffer (cmdline nofb sau GRUB fara mod grafic)");
    }
    if(!virtio_gpu_ready()){
        panic("test_gpu: lipseste virtio-gpu (QEMU fara -device virtio-gpu-pci?)");
    }
    uint64_t cmds_before = virtio_gpu_cmd_count();
    uint64_t free_before = pmm_free_count();
    uint64_t procs_before = process_count();
    uint64_t tasks_before = task_count();

    static const char* const argv[] = {"gputest", nullptr};
    uint64_t pid = exec_spawn("/bin/gputest", argv, envp);
    if(!pid){
        panic("test_gpu: /bin/gputest nu a pornit");
    }
    while(task_count() > tasks_before){
        yield();
        __asm__ volatile("hlt");
    }
    int code;
    if(!process_exit_status(pid, code)){
        panic("test_gpu: gputest (pid %lu) a disparut fara exit", pid);
    }
    if(code != 0){
        panic("test_gpu: gputest a iesit cu %d (vezi liniile \"gputest: FAIL\")", code);
    }
    if(virtio_gpu_user_owned()){
        panic("test_gpu: resursa virtio-gpu a ramas mapata");
    }
    if(virtio_gpu_cmd_count() < cmds_before + 100){
        panic("test_gpu: prea putine comenzi virtio-gpu (%lu)", virtio_gpu_cmd_count() - cmds_before);
    }
    if(fb_user_owned()){
        panic("test_gpu: dupa ycomp ecranul a ramas in modul grafic");
    }
    if(yinput_grabbed()){
        panic("test_gpu: captura de input a ramas");
    }
    if(shm_count() != 0){
        panic("test_gpu: %lu obiecte shm ramase", shm_count());
    }
    if(unix_count() != 0){
        panic("test_gpu: %lu socket-uri AF_UNIX ramase", unix_count());
    }
    if(process_count() != procs_before){
        panic("test_gpu: %ld procese ramase", (int64_t)(process_count() - procs_before));
    }
    if(pmm_free_count() + 16 < free_before){
        panic("test_gpu: %ld pagini pierdute", (int64_t)(free_before - pmm_free_count()));
    }
    kprintf("[OK] test_gpu (virtio-gpu 2D, ycomp pe gpu si pe fb)\n");
}

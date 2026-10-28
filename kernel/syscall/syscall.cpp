#include "syscall/syscall.h"
#include "syscall/table.h"
#include "uapi/syscall_nr.h"
#include "arch/x86_64/gdt.h"
#include "sched/sched.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "lib/string.h"

extern "C" void syscall_entry();

namespace {

const uint32_t MSR_EFER = 0xC0000080;
const uint32_t MSR_STAR = 0xC0000081;
const uint32_t MSR_LSTAR = 0xC0000082;
const uint32_t MSR_SFMASK = 0xC0000084;
const uint32_t MSR_GS_BASE = 0xC0000101;
const uint32_t MSR_KERNEL_GS_BASE = 0xC0000102;
const uint64_t EFER_SCE = 1UL << 0;
const uint64_t RFLAGS_TF = 1UL << 8;
const uint64_t RFLAGS_IF = 1UL << 9;
const uint64_t RFLAGS_DF = 1UL << 10;

// syscall incarca CS = STAR[47:32], SS = +8; sysret: SS = STAR[63:48] + 8, CS = STAR[63:48] + 16
const uint64_t STAR_KERNEL = GDT_KCODE;
const uint64_t STAR_USER = GDT_UDATA - 8;
static_assert(GDT_KDATA == GDT_KCODE + 8 && GDT_UCODE == GDT_UDATA + 8, "ordinea din GDT ceruta de syscall/sysret");

// Citita din syscall.S prin %gs
struct PerCpu {
    uint64_t kstack_top;
    uint64_t user_rsp;
};
static_assert(__builtin_offsetof(PerCpu, kstack_top) == 0 && __builtin_offsetof(PerCpu, user_rsp) == 8,
              "PerCpu trebuie sa corespunda cu syscall.S");

PerCpu percpu;

uint64_t rdmsr(uint32_t msr){
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

void wrmsr(uint32_t msr, uint64_t v){
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

}

void syscall_init(){
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR, (STAR_USER << 48) | (STAR_KERNEL << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF);
    // In kernel GS arata spre percpu; baza user (0) sta in KERNEL_GS_BASE pana la swapgs
    wrmsr(MSR_GS_BASE, (uint64_t)&percpu);
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    percpu.kstack_top = (uint64_t)task_current()->kstack + TASK_KSTACK_SIZE;
    kprintf("[OK] syscall (LSTAR %lx, STAR %lx)\n", rdmsr(MSR_LSTAR), rdmsr(MSR_STAR));
}

void syscall_set_kstack(uint64_t top){
    percpu.kstack_top = top;
}

extern "C" void syscall_dispatch(InterruptFrame* f){
    // Stiva de kernel e a task-ului, deci preemptia in timpul unui syscall e permisa
    __asm__ volatile("sti");
    SyscallFn fn = syscall_lookup(f->rax);
    f->rax = fn ? (uint64_t)fn(f->rdi, f->rsi, f->rdx, f->r10, f->r8, f->r9) : (uint64_t)-ENOSYS;
}

bool user_access_ok(uint64_t addr, uint64_t len, bool write){
    if(len == 0){
        return true;
    }
    uint64_t end = addr + len;
    if(end < addr || end > USER_TOP){
        return false;
    }
    const AddressSpace& as = *task_current()->as;
    uint64_t need = PTE_PRESENT | PTE_USER | (write ? PTE_RW : 0);
    for(uint64_t page = addr & ~(PAGE_4K - 1); page < end; page += PAGE_4K){
        if((vmm_entry(as, page) & need) != need){
            return false;
        }
    }
    return true;
}

bool copy_from_user(void* dst, uint64_t src, uint64_t len){
    if(!user_access_ok(src, len, false)){
        return false;
    }
    memcpy(dst, (const void*)src, len);
    return true;
}

bool copy_to_user(uint64_t dst, const void* src, uint64_t len){
    if(!user_access_ok(dst, len, true)){
        return false;
    }
    memcpy((void*)dst, src, len);
    return true;
}

int64_t copy_string_from_user(char* dst, uint64_t src, uint64_t max){
    for(uint64_t i = 0; i < max; i++){
        // O verificare pe pagina: restul paginii e accesibil daca primul byte e
        if((i == 0 || ((src + i) & (PAGE_4K - 1)) == 0) && !user_access_ok(src + i, 1, false)){
            return -EFAULT;
        }
        dst[i] = *(const char*)(src + i);
        if(!dst[i]){
            return (int64_t)i;
        }
    }
    return -E2BIG;
}

InterruptFrame* syscall_frame(){
    Task* t = task_current();
    return (InterruptFrame*)(t->kstack + TASK_KSTACK_SIZE - sizeof(InterruptFrame));
}

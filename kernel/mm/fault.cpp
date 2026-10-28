#include "mm/fault.h"
#include "mm/pmm.h"
#include "core/panic.h"
#include "lib/kprintf.h"
#include "init.h"
#include "arch/x86_64/percpu.h"

extern "C" char _kernel_start[];
extern "C" char _rodata_start[];
extern "C" char _data_start[];
extern "C" char _kernel_end[];

namespace {

const uint8_t VEC_PF = 14;

uint64_t read_cr2(){
    uint64_t v;
    __asm__ volatile("mov %%cr2, %0" : "=r"(v));
    return v;
}

AddressSpace current_as(){
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return AddressSpace{cr3 & PTE_ADDR_MASK};
}

const char* access_kind(uint64_t err){
    if(err & PF_ERR_FETCH){
        return "executie";
    }
    return err & PF_ERR_WRITE ? "scriere" : "citire";
}

void page_fault(InterruptFrame* f){
    uint64_t addr = read_cr2();
    uint64_t err = f->error;
    AddressSpace as = current_as();
    if(vma_fault(as, addr, err)){
        return;
    }
    const char* cause = err & PF_ERR_PRESENT ? "violare de protectie" : "pagina nemapata";
    if(err & PF_ERR_USER){
        // interrupt_dispatch omoara procesul inainte de intoarcerea in ring 3 (pe acelasi CPU)
        this_cpu()->kill_pending = true;
        kprintf("page fault user: %s la %lx (%s)\n", access_kind(err), addr, cause);
        return;
    }
    panic_frame(f, "page fault in kernel: %s la %lx (%s) [P=%d W=%d U=%d I=%d]",
                access_kind(err), addr, cause, (int)(err & PF_ERR_PRESENT), (int)((err & PF_ERR_WRITE) >> 1),
                (int)((err & PF_ERR_USER) >> 2), (int)((err & PF_ERR_FETCH) >> 4));
}

// Fiecare pagina din [start, end) trebuie mapata cu exact bitii RW/NX ceruti
bool check_range(uint64_t start, uint64_t end, uint64_t want){
    for(uint64_t v = start; v < end; v += PAGE_SIZE){
        uint64_t e = vmm_entry(kernel_as, v);
        if(!e || (e & (PTE_RW | PTE_NX)) != want){
            kprintf("protectie gresita la %lx: entry=%lx\n", v, e);
            return false;
        }
    }
    return true;
}

}

void fault_init(){
    irq_register(VEC_PF, page_fault);
    kprintf("[OK] pagefault\n");
}

bool vma_fault(AddressSpace&, uint64_t, uint64_t){
    return false;
}

bool fault_kill_pending(){
    return this_cpu()->kill_pending;
}

void fault_kill_clear(){
    this_cpu()->kill_pending = false;
}

void fault_protect_kernel(){
    uint64_t init_start = (uint64_t)_init_start;
    uint64_t init_end = (uint64_t)_init_end;
    for(uint64_t v = init_start; v < init_end; v += PAGE_SIZE){
        vmm_unmap(kernel_as, v);
        pmm_free_page(v - KERNEL_VMA);
    }
    for(uint64_t v = init_start; v < init_end; v += PAGE_SIZE){
        if(vmm_translate(kernel_as, v)){
            panic("fault_protect_kernel: .init.text inca mapat la %lx", v);
        }
    }

    // NX se verifica doar daca CPU-ul il are (EFER.NXE)
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080));
    bool has_nx = lo & (1u << 11);
    uint64_t nx = has_nx ? PTE_NX : 0;
    if(!check_range((uint64_t)_kernel_start, init_start, 0) ||
       !check_range(init_end, (uint64_t)_rodata_start, 0) ||
       !check_range((uint64_t)_rodata_start, (uint64_t)_data_start, nx) ||
       !check_range((uint64_t)_data_start, (uint64_t)_kernel_end, PTE_RW | nx)){
        panic("fault_protect_kernel: protectiile kernelului nu sunt cele asteptate");
    }
    kprintf("[OK] protect (.init %lu KiB eliberat, text RX, rodata R%s, data RW%s)\n",
            (init_end - init_start) >> 10, has_nx ? "+NX" : "", has_nx ? "+NX" : "");
}

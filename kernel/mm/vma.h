#pragma once
#include "types.h"
#include "mm/vmm.h"

struct File;
struct Process;

// Flaguri suplimentare ale unui Vma (peste MAP_SHARED / MAP_PRIVATE / MAP_ANONYMOUS)
const uint32_t VMA_HEAP = 1u << 16;         // heap-ul brk
const uint32_t VMA_STACK = 1u << 17;        // stiva (creste lent prin page fault)
const uint32_t VMA_SYSTEM = 1u << 18;       // trambulina de semnale: nu se poate muta / sterge din user
const uint32_t VMA_DEVICE = 1u << 19;       // memorie de dispozitiv (/dev/fb0, MAP_SHARED): pagini fizice fixe, PTE_DEV
const uint32_t VMA_SHMOBJ = 1u << 20;       // obiect shm_open (ipc/shm.h): paginile obiectului, cu pageref, PTE_SHM

// O zona contigua din spatiul user, cu aceleasi drepturi si aceeasi sursa. Lista e sortata dupa start si fara suprapuneri.
// Paginile se aduc la primul acces (page fault): anonim = zero, fisier = citit din inode la off + (addr - start)
struct Vma {
    uint64_t start, end;            // aliniate la pagina, end exclusiv
    uint32_t prot;                  // PROT_*
    uint32_t flags;
    File* file;                     // tinut cu referinta; nullptr pentru anonim
    uint64_t off;                   // offset in fisier al lui start
    Vma* next;
};

struct VmaList {
    Vma* head;
    uint64_t count;
};

// Zona care contine addr, nullptr daca nu e nicio zona acolo
Vma* vma_find(const VmaList& l, uint64_t addr);
// Zona noua [start, end) (fara suprapuneri cu cele existente); ia o referinta la file. -ENOMEM
int vma_insert(VmaList& l, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags, File* file, uint64_t off);
// Toate zonele, cu referintele la fisiere (paginile nu se ating)
void vma_list_free(VmaList& l);
// Copie pentru fork; -ENOMEM
int vma_list_clone(VmaList& dst, const VmaList& src);
// Procesul curent iese / face exec: scrie inapoi paginile murdare MAP_SHARED din fisiere si elibereaza lista
void vma_exit(Process* p);

// Syscall-urile de memorie ale procesului curent (0 / adresa sau -errno)
int64_t vma_mmap(uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags, int64_t fd, uint64_t off);
int vma_munmap(uint64_t addr, uint64_t len);
int vma_mprotect(uint64_t addr, uint64_t len, uint64_t prot);
int vma_msync(uint64_t addr, uint64_t len, uint64_t flags);
// brk: intoarce noul break (sau cel vechi daca cererea nu se poate)
uint64_t vma_brk(uint64_t addr);

// Rezolva un page fault pe addr in procesul curent (pagina la cerere, CoW); false = SIGSEGV
bool vma_fault(AddressSpace& as, uint64_t addr, uint64_t err);
// Pentru copy_to/from_user: aduce pagina lui addr in memorie (si o face scriibila / sparge CoW daca write)
bool vma_fault_in(uint64_t addr, bool write);

// Elibereaza paginile fizice mapate in [from, to) (cele lipsa se sar)
void vma_unmap_pages(AddressSpace& as, uint64_t from, uint64_t to);

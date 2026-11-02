#pragma once
#include "types.h"
#include "interrupt.h"
#include "sched/task.h"
#include "sync/waitqueue.h"
#include "proc/file.h"
#include "fs/vfs/path.h"
#include "mm/vma.h"
#include "proc/signal.h"
#include "proc/cred.h"

enum class ProcState : uint8_t { ALIVE, ZOMBIE };

// Proces user: un spatiu de adrese, un tabel de fd si un singur thread (main). pid = tid-ul thread-ului
// principal. Structura traieste pana cand parintele face waitpid (sau, fara parinte, pana se elibereaza task-ul)
struct Process {
    uint64_t pid;
    uint64_t ppid;                  // 0 daca parintele s-a terminat sau procesul a fost pornit de kernel
    ProcState state;
    bool reaped;                    // waitpid a luat deja starea
    int exit_code;                  // exit(cod), sau 128 + semnal daca a fost omorat
    int wait_status;                // pentru wait4, ca pe Linux (W_EXITCODE sau numarul semnalului)
    Task* main;                     // nullptr dupa ce task-ul a fost eliberat
    Process* parent;
    ListLink children;
    ListLink sibling;
    FdTable fds;
    Path cwd;                       // dentry == nullptr = radacina VFS; referinta tinuta pana la exit
    uint64_t brk_base;              // brk: de la sfarsitul imaginii ELF; paginile pana la brk_cur sunt mapate
    uint64_t brk_cur;
    VmaList vmas;                   // zonele de memorie user (mm/vma.h), sortate dupa adresa
    uint64_t pgid;                  // grupul de procese (Ctrl+C merge la grupul din prim-plan al terminalului)
    SignalState sig;                // semnale: actiuni, masca, pending (proc/signal.h)
    Cred cred;                      // uid/gid reale, efective, salvate + grupuri (proc/cred.h); mostenit la fork
    uint32_t umask;                 // bitii scosi din mode la creare (implicit 022)
    WaitQueue child_exit;           // parintele asteapta aici in waitpid
    char name[TASK_NAME_LEN];
    uint16_t cmdline_len;           // argv separat prin '\0', trunchiat la PROC_CMDLINE_MAX
    char cmdline[PROC_CMDLINE_MAX];
};

// Proces nou pentru task-ul main (care are deja spatiul lui de adrese); fd-urile sunt goale.
// parent nullptr = pornit de kernel. nullptr fara memorie
Process* process_create(Task* main, Process* parent);
// Elibereaza un proces care nu a pornit niciodata (main inca nepornit, fara copii)
void process_abort(Process* p);
// Elibereaza structura unui proces scos deja din arbore, al carui task nu mai exista
void process_free(Process* p);
Process* process_current();
void process_set_name(Process* p, const char* name);
// Pentru /proc/<pid>/cmdline; argv terminat cu nullptr (poate fi nullptr)
void process_set_cmdline(Process* p, const char* const* argv);

// Termina procesul curent cu codul dat (exit sau fault fatal): inchide fd-urile, devine ZOMBIE si
// trezeste parintele. Nu revine
[[noreturn]] void process_exit(int code);
// Procesul curent moare din cauza semnalului sig (iesire 128 + sig, wait4 vede semnalul). Nu revine
[[noreturn]] void process_die(int sig);
// Din task_destroy: thread-ul principal nu mai exista
void process_task_destroyed(Task* t);

// fork(): copie completa a spatiului user si a tabelului de fd; copilul revine din syscall cu 0.
// pid-ul copilului sau -errno (fork.cpp)
int64_t process_fork(const InterruptFrame& parent_frame);
// waitpid/wait4 (wait.cpp): pid -1 = orice copil; status in formatul wait4; options WNOHANG.
// pid-ul copilului terminat, 0 (WNOHANG, niciunul gata) sau -ECHILD
int64_t process_waitpid(int64_t pid, int* status, int options);

// Codul de iesire al unui proces terminat recent (ultimele 16), pentru teste; false daca nu e gasit
bool process_exit_status(uint64_t pid, int& code);
// Cate structuri Process exista (pentru teste: nu raman procese neeliberate)
uint64_t process_count();

// Lock-ul arborelui de procese (parinte/copii/stare), luat cu lock_irqsave
Spinlock& process_tree_lock();

#ifndef _SYS_RTSTATS_H
#define _SYS_RTSTATS_H
#include <stdint.h>
/* Copie a kernel/include/uapi/rtstats.h */
/* rt_stats(&s) (T10.2): incarcarea clasei RT, pentru programe care vor sa se dea la o parte (compozitorul).
 * load_ppm = timpul CPU folosit de task-urile RT (SCHED_FIFO din user si task-urile periodice ale kernelului) in
 * ultima fereastra de window_ms, in milionimi dintr-un CPU (1000000 = un CPU intreg; cu mai multe CPU-uri poate trece
 * peste). Fereastra se inchide la un apel care vine dupa cel putin 200 ms; apelurile mai dese primesc aceeasi valoare.
 * util_ppm = utilizarea admisa a task-urilor periodice (suma wcet/perioada). */
struct yk_rt_stats {
    uint32_t ncpu;
    uint32_t periodic;          /* task-uri periodice */
    uint32_t util_ppm;
    uint32_t load_ppm;
    uint64_t window_ms;
    uint64_t rt_busy_ms;        /* total de la boot, toate CPU-urile */
    uint64_t deadline_misses;   /* total al task-urilor periodice care exista acum */
};

int rt_stats(struct yk_rt_stats *s);
#endif

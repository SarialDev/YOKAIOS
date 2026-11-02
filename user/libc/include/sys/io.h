#ifndef _SYS_IO_H
#define _SYS_IO_H
/* ioperm(from, num, on): acces direct in ring 3 la porturile [from, from + num); cere CAP_RAWIO. Nu se mosteneste la fork,
 * dispare la exec. Fara permisiune, in/out dau #GP (procesul primeste SIGSEGV) */
int ioperm(unsigned long from, unsigned long num, int on);
static inline void outb(unsigned char v, unsigned short port){
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port));
}
static inline unsigned char inb(unsigned short port){
    unsigned char v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
#endif

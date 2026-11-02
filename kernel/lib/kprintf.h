#pragma once

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)
#define va_end(ap)         __builtin_va_end(ap)

void kputc(char c);
// n octeti pe consola cu un singur lock (nu se amesteca cu alte CPU-uri), apoi cursorul vizibil se aliniaza la pozitia de scriere
void kwrite(const char* s, unsigned long n);
void kprintf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void kvprintf(const char* fmt, va_list ap);
// Panic: elibereaza fortat lock-ul consolei daca il tine alt CPU (oprit intre timp)
void console_break_lock();

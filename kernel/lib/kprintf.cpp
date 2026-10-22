#include "lib/kprintf.h"
#include "drivers/serial.h"
#include "drivers/vga.h"
#include "types.h"

void kputc(char c){
    serial_putc(c);
    vga_putc(c);
}

static void put_padded(const char* s, int len, int width, char pad){
    for(int i = len; i < width; i++){
        kputc(pad);
    }
    for(int i = 0; i < len; i++){
        kputc(s[i]);
    }
}

static void put_number(uint64_t v, unsigned base, bool negative, int width, char pad){
    char buf[24];
    int len = 0;
    do {
        unsigned d = v % base;
        buf[len++] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while(v);

    if(negative){
        if(pad == '0'){
            kputc('-');
            width--;
        } else {
            buf[len++] = '-';
        }
    }

    char out_buf[24];
    for(int i = 0; i < len; i++){
        out_buf[i] = buf[len - 1 - i];
    }
    put_padded(out_buf, len, width, pad);
}

void kvprintf(const char* fmt, va_list ap){
    for(; *fmt; fmt++){
        if(*fmt != '%'){
            kputc(*fmt);
            continue;
        }
        fmt++;

        char pad = ' ';
        if(*fmt == '0'){
            pad = '0';
            fmt++;
        }
        int width = 0;
        while(*fmt >= '0' && *fmt <= '9'){
            width = width * 10 + (*fmt++ - '0');
        }
        bool is_long = false;
        while(*fmt == 'l'){
            is_long = true;
            fmt++;
        }

        switch(*fmt){
            case 's': {
                const char* s = va_arg(ap, const char*);
                if(!s){
                    s = "(null)";
                }
                int len = 0;
                while(s[len]){
                    len++;
                }
                put_padded(s, len, width, ' ');
                break;
            }
            case 'c':
                kputc((char)va_arg(ap, int));
                break;
            case 'd':
            case 'i': {
                int64_t v = is_long ? va_arg(ap, int64_t) : va_arg(ap, int);
                bool neg = v < 0;
                put_number(neg ? (uint64_t)0 - (uint64_t)v : (uint64_t)v, 10, neg, width, pad);
                break;
            }
            case 'u':
                put_number(is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, false, width, pad);
                break;
            case 'x':
                put_number(is_long ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, false, width, pad);
                break;
            case 'p':
                kputc('0');
                kputc('x');
                put_number((uintptr_t)va_arg(ap, void*), 16, false, 16, '0');
                break;
            case '%':
                kputc('%');
                break;
            case '\0':
                return;
            default:
                kputc('%');
                kputc(*fmt);
                break;
        }
    }
}

void kprintf(const char* fmt, ...){
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
}

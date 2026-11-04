/* Terminalul: mod brut, citirea tastelor (ESC [ ... ca pe ANSI), iesire cu buffer */
#include "yedit.h"
#include <unistd.h>
#include <ctype.h>
#include <termios.h>
#include <sys/ioctl.h>

static struct termios saved_tio, raw_tio;
static int is_tty;
static int pushed = -1;                 /* un octet citit prea devreme dupa ESC */

static char obuf[8192];
static size_t olen;

void out_flush(void){
    size_t off = 0;
    while(off < olen){
        ssize_t w = write(1, obuf + off, olen - off);
        if(w <= 0){
            break;
        }
        off += (size_t)w;
    }
    olen = 0;
}

void out_put(char c){
    if(olen == sizeof(obuf)){
        out_flush();
    }
    obuf[olen++] = c;
}

void out_bytes(const char *s, size_t n){
    for(size_t i = 0; i < n; i++){
        out_put(s[i]);
    }
}

void out_str(const char *s){
    while(*s){
        out_put(*s++);
    }
}

int term_open(void){
    if(tcgetattr(0, &saved_tio) == 0){
        is_tty = 1;
        raw_tio = saved_tio;
        raw_tio.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
        raw_tio.c_cc[VMIN] = 1;
        raw_tio.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &raw_tio);
    }
    return is_tty;
}

void term_close(void){
    out_flush();
    if(is_tty){
        tcsetattr(0, TCSANOW, &saved_tio);
    }
}

void term_size(int *rows, int *cols){
    struct winsize ws;
    *rows = 25;
    *cols = 80;
    if((ioctl(1, TIOCGWINSZ, &ws) == 0 || ioctl(0, TIOCGWINSZ, &ws) == 0) && ws.ws_row >= 3 && ws.ws_col >= 10){
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    }
}

static void set_vmin(int v){
    if(is_tty){
        raw_tio.c_cc[VMIN] = (cc_t)v;
        tcsetattr(0, TCSANOW, &raw_tio);
    }
}

/* Octetul de dupa ESC (VMIN=0: secventa vine intreaga, un ESC singur nu trebuie sa blocheze); -1 daca nu mai vine nimic */
static int esc_next(void){
    for(int tries = 0; tries < 4; tries++){
        unsigned char c;
        if(read(0, &c, 1) == 1){
            return c;
        }
        usleep(3000);
    }
    return -1;
}

static int parse_escape(void){
    int b = esc_next();
    if(b < 0){
        return K_ESC;
    }
    if(b == '['){
        int params[2] = {0, 0};
        int np = 0;
        int ch;
        while((ch = esc_next()) >= 0){
            if(isdigit(ch)){
                params[np] = params[np] * 10 + (ch - '0');
            } else if(ch == ';'){
                if(np < 1){
                    np++;
                }
            } else {
                break;
            }
        }
        switch(ch){
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        case '~':
            switch(params[0]){
            case 1: case 7: return K_HOME;
            case 4: case 8: return K_END;
            case 3: return K_DEL;
            case 5: return K_PGUP;
            case 6: return K_PGDN;
            default: return K_IGNORE;
            }
        default: return K_IGNORE;
        }
    }
    if(b == 'O'){
        switch(esc_next()){
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        case 'H': return K_HOME;
        case 'F': return K_END;
        default: return K_IGNORE;
        }
    }
    pushed = b;                         /* ESC urmat de alta tasta: ESC singur, tasta ramane pentru urmatorul apel */
    return K_ESC;
}

int term_key(void){
    if(pushed >= 0){
        int k = pushed;
        pushed = -1;
        return k;
    }
    unsigned char c;
    if(read(0, &c, 1) != 1){
        return K_EOF;
    }
    if(c != 27){
        return c;
    }
    set_vmin(0);
    int k = parse_escape();
    set_vmin(1);
    return k;
}

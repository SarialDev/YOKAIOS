/* authdb.h: partea comuna a login si passwd (inclus, nu e biblioteca): fisierele /etc/shadow si /etc/auth, citirea
 * secretelor de la terminal (fara ecou), sare din /dev/random. Hash-urile au formatul din <crypto/pwhash.h>.
 *   /etc/shadow : nume:hash:ultima_schimbare:min:max:avertizare:inactiv:expira:   (ca pe Linux; hash "*" sau "!" = fara parola)
 *   /etc/auth   : nume:pin:hash                    PIN-ul
 *                 nume:key:/media/...:hash         fisier-cheie: hash-ul CONTINUTULUI fisierului (cel mult KEY_MAX octeti)
 * Ambele sunt 0600, ale lui root. Scrierea merge prin fisier temporar + rename. */
#ifndef YOKAI_AUTHDB_H
#define YOKAI_AUTHDB_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <termios.h>
#include <sys/stat.h>
#include <crypto/pwhash.h>

#define SHADOW_FILE "/etc/shadow"
#define AUTH_FILE   "/etc/auth"
#define KEY_PREFIX  "/media/"       /* fisierele-cheie stau doar pe suporturi montate */
#define KEY_MAX     4096
#define ADB_FILE_MAX 65536
#define ADB_LINE_MAX 512
#define SECRET_MAX  128

#define ADB static __attribute__((unused))

/* n octeti din /dev/random; 0 la succes */
ADB int adb_random(void *buf, size_t n){
    int fd = open("/dev/random", O_RDONLY);
    if(fd < 0){
        return -1;
    }
    size_t got = 0;
    while(got < n){
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if(r <= 0){
            close(fd);
            return -1;
        }
        got += (size_t)r;
    }
    close(fd);
    return 0;
}

/* Hash nou (sare aleatoare, PWHASH_ITER_DEFAULT iteratii) pentru secret; out are cel putin PWHASH_MAX octeti */
ADB int adb_hash_new(const void *secret, size_t len, char *out){
    uint8_t salt[PWHASH_SALT_LEN];
    if(adb_random(salt, sizeof(salt)) < 0){
        return -1;
    }
    return pwhash_make(secret, len, salt, PWHASH_ITER_DEFAULT, out, PWHASH_MAX);
}

/* Calea de fisier-cheie e acceptata? Trebuie sa inceapa cu prefix si sa nu iasa din el prin ".." */
ADB int adb_key_path_ok(const char *path, const char *prefix){
    size_t n = strlen(path);
    if(strncmp(path, prefix, strlen(prefix)) != 0 || n >= 256){
        return 0;
    }
    if(strstr(path, "/../") || (n >= 3 && strcmp(path + n - 3, "/..") == 0)){
        return 0;
    }
    return 1;
}

/* Continutul fisierului-cheie in buf (cel mult KEY_MAX octeti, nenul); lungimea sau -1 */
ADB long adb_read_key(const char *path, char *buf){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return -1;
    }
    size_t len = 0;
    for(;;){
        ssize_t r = read(fd, buf + len, KEY_MAX + 1 - len);
        if(r < 0){
            close(fd);
            return -1;
        }
        if(r == 0){
            break;
        }
        len += (size_t)r;
        if(len > KEY_MAX){
            close(fd);
            return -1;
        }
    }
    close(fd);
    return len ? (long)len : -1;
}

/* Tot fisierul in memorie (terminat cu \0); NULL daca nu exista (errno) */
ADB char *adb_slurp(const char *path, size_t *len){
    int fd = open(path, O_RDONLY);
    if(fd < 0){
        return NULL;
    }
    char *buf = malloc(ADB_FILE_MAX + 1);
    if(!buf){
        close(fd);
        errno = ENOMEM;
        return NULL;
    }
    size_t n = 0;
    for(;;){
        ssize_t r = read(fd, buf + n, ADB_FILE_MAX - n);
        if(r < 0){
            free(buf);
            close(fd);
            return NULL;
        }
        if(r == 0){
            break;
        }
        n += (size_t)r;
        if(n >= ADB_FILE_MAX){
            free(buf);
            close(fd);
            errno = EFBIG;
            return NULL;
        }
    }
    close(fd);
    buf[n] = 0;
    *len = n;
    return buf;
}

/* Imparte linia (modificata pe loc) la ':' in cel mult max campuri; numarul lor */
ADB int adb_split(char *line, char **f, int max){
    int n = 0;
    f[n++] = line;
    for(char *p = line; *p && n < max; p++){
        if(*p == ':'){
            *p = 0;
            f[n++] = p + 1;
        }
    }
    return n;
}

/* Cauta intrarea utilizatorului: kind == NULL -> /etc/shadow (hash = campul 2); "pin" -> hash = campul 3;
 * "key" -> path = campul 3, hash = campul 4. Intoarce 1 daca exista, 0 daca nu (si daca fisierul lipseste), -1 la alta eroare */
ADB int adb_lookup(const char *file, const char *user, const char *kind, char *hash, size_t hcap, char *path, size_t pcap){
    size_t len;
    char *text = adb_slurp(file, &len);
    if(!text){
        return errno == ENOENT ? 0 : -1;
    }
    int found = 0;
    for(char *cur = text; *cur && !found;){
        char *nl = strchr(cur, '\n');
        char line[ADB_LINE_MAX];
        size_t ll = nl ? (size_t)(nl - cur) : strlen(cur);
        char *next = nl ? nl + 1 : cur + ll;
        if(ll < sizeof(line) && cur[0] != '#'){
            memcpy(line, cur, ll);
            line[ll] = 0;
            char *f[9];
            int nf = adb_split(line, f, 9);
            if(nf >= 2 && strcmp(f[0], user) == 0){
                if(!kind && nf >= 2){
                    snprintf(hash, hcap, "%s", f[1]);
                    found = 1;
                } else if(kind && strcmp(kind, "pin") == 0 && nf >= 3 && strcmp(f[1], "pin") == 0){
                    snprintf(hash, hcap, "%s", f[2]);
                    found = 1;
                } else if(kind && strcmp(kind, "key") == 0 && nf >= 4 && strcmp(f[1], "key") == 0){
                    if(path){
                        snprintf(path, pcap, "%s", f[2]);
                    }
                    snprintf(hash, hcap, "%s", f[3]);
                    found = 1;
                }
            }
        }
        cur = next;
    }
    free(text);
    return found;
}

/* Scrie/inlocuieste intrarea utilizatorului (aceleasi reguli ca la adb_lookup; path doar pentru "key"). La /etc/shadow
 * pastreaza campurile min/max/avertizare... ale liniei vechi si pune data schimbarii. 0 la succes, -1 (errno) altfel */
ADB int adb_store(const char *file, const char *user, const char *kind, const char *path, const char *hash){
    size_t len = 0;
    char *old = adb_slurp(file, &len);
    if(!old){
        if(errno != ENOENT){
            return -1;
        }
        old = calloc(1, 1);
        if(!old){
            errno = ENOMEM;
            return -1;
        }
    }
    char *out = malloc(len + 2 * ADB_LINE_MAX);
    if(!out){
        free(old);
        errno = ENOMEM;
        return -1;
    }
    size_t on = 0;
    int done = 0;
    char entry[ADB_LINE_MAX];
    long days = (long)(time(NULL) / 86400);
    for(char *cur = old; *cur;){
        char *nl = strchr(cur, '\n');
        size_t ll = nl ? (size_t)(nl - cur) : strlen(cur);
        char *next = nl ? nl + 1 : cur + ll;
        char line[ADB_LINE_MAX];
        int match = 0;
        char *f[9] = {0};
        int nf = 0;
        if(ll < sizeof(line) && cur[0] != '#'){
            memcpy(line, cur, ll);
            line[ll] = 0;
            nf = adb_split(line, f, 9);
            match = nf >= 2 && strcmp(f[0], user) == 0 && (!kind || (strcmp(f[1], kind) == 0));
        }
        if(match && !done){
            if(!kind){
                snprintf(entry, sizeof(entry), "%s:%s:%ld:%s:%s:%s:%s:%s:%s\n", user, hash, days,
                         nf > 3 ? f[3] : "0", nf > 4 ? f[4] : "99999", nf > 5 ? f[5] : "7",
                         nf > 6 ? f[6] : "", nf > 7 ? f[7] : "", nf > 8 ? f[8] : "");
            } else if(path){
                snprintf(entry, sizeof(entry), "%s:%s:%s:%s\n", user, kind, path, hash);
            } else {
                snprintf(entry, sizeof(entry), "%s:%s:%s\n", user, kind, hash);
            }
            size_t el = strlen(entry);
            memcpy(out + on, entry, el);
            on += el;
            done = 1;
        } else if(!match){
            memcpy(out + on, cur, ll);
            on += ll;
            out[on++] = '\n';
        }
        cur = next;
    }
    if(!done){
        if(!kind){
            snprintf(entry, sizeof(entry), "%s:%s:%ld:0:99999:7:::\n", user, hash, days);
        } else if(path){
            snprintf(entry, sizeof(entry), "%s:%s:%s:%s\n", user, kind, path, hash);
        } else {
            snprintf(entry, sizeof(entry), "%s:%s:%s\n", user, kind, hash);
        }
        size_t el = strlen(entry);
        memcpy(out + on, entry, el);
        on += el;
    }
    char tmp[300];
    snprintf(tmp, sizeof(tmp), "%s.new", file);
    int rc = -1;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if(fd >= 0){
        size_t w = 0;
        int ok = 1;
        while(w < on){
            ssize_t r = write(fd, out + w, on - w);
            if(r <= 0){
                ok = 0;
                break;
            }
            w += (size_t)r;
        }
        int e = errno;
        close(fd);
        if(ok && rename(tmp, file) == 0){
            rc = 0;
        } else {
            if(!ok){
                errno = e;
            }
            e = errno;
            unlink(tmp);
            errno = e;
        }
    }
    int e = errno;
    free(old);
    free(out);
    errno = e;
    return rc;
}

/* ---- citire de la terminal ---- */
static struct termios adb_saved_tio;
static int adb_tio_saved;

ADB void adb_tty_restore(void){
    if(adb_tio_saved){
        tcsetattr(0, TCSANOW, &adb_saved_tio);
        adb_tio_saved = 0;
    }
}

ADB void adb_on_int(int sig){
    (void)sig;
    adb_tty_restore();
    (void)!write(1, "\n", 1);
    _exit(130);
}

/* Afiseaza prompt si citeste o linie (fara \n) in buf; echo == 0: fara ecou cand stdin e terminal (ecoul revine si la Ctrl-C).
 * Citeste octet cu octet din fd 0 (nu foloseste stdio), deci merge la fel pe terminal si pe pipe/fisier.
 * Intoarce lungimea, -1 la EOF fara niciun octet, -2 daca linia nu incape (cap include \0) */
ADB int adb_read_line(const char *prompt, int echo, char *buf, size_t cap){
    fputs(prompt, stdout);
    fflush(stdout);
    int tty = isatty(0);
    int hide = !echo && tty && tcgetattr(0, &adb_saved_tio) == 0;
    if(hide){
        struct termios t = adb_saved_tio;
        t.c_lflag &= ~(tcflag_t)ECHO;
        adb_tio_saved = 1;
        signal(SIGINT, adb_on_int);
        tcsetattr(0, TCSANOW, &t);
    }
    size_t n = 0;
    int over = 0, got_any = 0;
    for(;;){
        char c;
        ssize_t r = read(0, &c, 1);
        if(r <= 0){
            break;
        }
        got_any = 1;
        if(c == '\n'){
            break;
        }
        if(c == '\r'){
            continue;
        }
        if(n + 1 < cap){
            buf[n++] = c;
        } else {
            over = 1;
        }
    }
    buf[n] = 0;
    if(hide){
        adb_tty_restore();
        signal(SIGINT, SIG_DFL);
        fputs("\n", stdout);
        fflush(stdout);
    }
    if(over){
        return -2;
    }
    return got_any ? (int)n : -1;
}

#endif

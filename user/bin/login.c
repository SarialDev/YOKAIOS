/* login [-p | -k | -K FISIER] [-c COMANDA] [-s SHADOW] [-a AUTH] [-M PREFIX] [utilizator]
 * Autentifica un utilizator si ii porneste shell-ul (ca su -): initgroups + setgid + setuid, HOME, SHELL, USER, LOGNAME.
 *   implicit : parola, comparata cu hash-ul din /etc/shadow
 *   -p       : PIN, hash in /etc/auth ("nume:pin:hash")
 *   -k       : fisier-cheie; calea e cea din /etc/auth ("nume:key:/media/...:hash"), hash-ul e al CONTINUTULUI fisierului
 *   -K cale  : ca -k, dar cu fisierul de la cale (tot sub /media/)
 * Hash-uri: $pbkdf2-sha256$iteratii$sare$hash (lib/crypto/pwhash). Compararea e in timp constant; un utilizator
 * inexistent sau blocat ("*", "!") cheltuieste acelasi timp de calcul ca unul real. Trei incercari (parola/PIN), 1 s pauza dupa fiecare esec.
 * Doar root il poate porni (init). -s -a -M (alt shadow / auth / prefix pentru fisierul-cheie) sunt pentru teste.
 * Fara PAM: o singura metoda pe rand. */
#include "authdb.h"
#include <pwd.h>
#include <grp.h>

/* Hash valid, fara parola cunoscuta: pentru utilizatori lipsa/blocati, ca timpul sa nu-i dea de gol */
#define DUMMY_HASH "$pbkdf2-sha256$100000$000102030405060708090a0b0c0d0e0f$844d6e092def956a3d2773e41d721bf2d95a1d8f524d970ca76cd20528a03679"

enum { M_PASS, M_PIN, M_KEY };

static const char *shadow_file = SHADOW_FILE;
static const char *auth_file = AUTH_FILE;
static const char *key_prefix = KEY_PREFIX;

static void usage(void){
    fprintf(stderr, "uz: login [-p | -k | -K fisier] [-c comanda] [utilizator]\n");
}

/* 1 = secretul (sau fisierul-cheie) e bun pentru utilizator */
static int check(const char *user, int mode, const void *secret, size_t len, const char *key_override){
    char hash[PWHASH_MAX + 16], path[256] = "";
    int found;
    if(mode == M_PASS){
        found = adb_lookup(shadow_file, user, NULL, hash, sizeof(hash), NULL, 0);
    } else if(mode == M_PIN){
        found = adb_lookup(auth_file, user, "pin", hash, sizeof(hash), NULL, 0);
    } else {
        found = adb_lookup(auth_file, user, "key", hash, sizeof(hash), path, sizeof(path));
    }
    int real = found == 1 && pwhash_valid(hash);
    static char keybuf[KEY_MAX + 1];
    if(mode == M_KEY){
        const char *p = key_override ? key_override : path;
        long n = real && adb_key_path_ok(p, key_prefix) ? adb_read_key(p, keybuf) : -1;
        if(n < 0){
            real = 0;
            n = 1;                  /* cheltuie totusi timpul unui hash */
            keybuf[0] = 0;
        }
        secret = keybuf;
        len = (size_t)n;
    } else if(len == 0){
        real = 0;                   /* fara parola/PIN gol, chiar daca asa scrie in fisier */
    }
    int r = pwhash_verify(secret, len, real ? hash : DUMMY_HASH);
    return real && r == 1;
}

static int start_session(const char *user, const char *cmd){
    struct passwd *pw = getpwnam(user);
    if(!pw){
        return 1;
    }
    char name[64], home[128], shell[128];
    snprintf(name, sizeof(name), "%s", pw->pw_name);
    snprintf(home, sizeof(home), "%s", pw->pw_dir);
    snprintf(shell, sizeof(shell), "%s", *pw->pw_shell ? pw->pw_shell : "/bin/ysh");
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    if(initgroups(name, gid) < 0 || setgid(gid) < 0 || setuid(uid) < 0){
        fprintf(stderr, "login: %s\n", strerror(errno));
        return 1;
    }
    setenv("USER", name, 1);
    setenv("LOGNAME", name, 1);
    setenv("HOME", home, 1);
    setenv("SHELL", shell, 1);
    setenv("PATH", "/bin:/sbin", 1);
    if(chdir(home) < 0){
        fprintf(stderr, "login: %s: %s (pornesc in /)\n", home, strerror(errno));
        setenv("HOME", "/", 1);
        chdir("/");
    }
    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    if(cmd){
        char *av[] = {(char *)base, "-c", (char *)cmd, NULL};
        execv(shell, av);
    } else {
        char *av[] = {(char *)base, "-l", NULL};
        execv(shell, av);
    }
    fprintf(stderr, "login: %s: %s\n", shell, strerror(errno));
    return 127;
}

int main(int argc, char **argv){
    int mode = M_PASS;
    const char *cmd = NULL, *key_override = NULL, *user = NULL;
    int i;
    for(i = 1; i < argc; i++){
        if(strcmp(argv[i], "--help") == 0){
            puts("uz: login [-p | -k | -K fisier] [-c comanda] [utilizator]");
            return 0;
        } else if(strcmp(argv[i], "-p") == 0){
            mode = M_PIN;
        } else if(strcmp(argv[i], "-k") == 0){
            mode = M_KEY;
        } else if(strcmp(argv[i], "-K") == 0 && i + 1 < argc){
            mode = M_KEY;
            key_override = argv[++i];
        } else if(strcmp(argv[i], "-c") == 0 && i + 1 < argc){
            cmd = argv[++i];
        } else if(strcmp(argv[i], "-s") == 0 && i + 1 < argc){
            shadow_file = argv[++i];
        } else if(strcmp(argv[i], "-a") == 0 && i + 1 < argc){
            auth_file = argv[++i];
        } else if(strcmp(argv[i], "-M") == 0 && i + 1 < argc){
            key_prefix = argv[++i];
        } else if(argv[i][0] != '-' && !user){
            user = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if(getuid() != 0 || geteuid() != 0){
        fprintf(stderr, "login: trebuie pornit ca root\n");
        return 1;
    }
    char uname_buf[64];
    int interactive = isatty(0);
    if(!user){
        const char *host = getenv("HOSTNAME");
        char prompt[96];
        snprintf(prompt, sizeof(prompt), "%s login: ", host && *host ? host : "yokai");
        if(adb_read_line(prompt, 1, uname_buf, sizeof(uname_buf)) <= 0){
            return 1;
        }
        user = uname_buf;
    }
    /* Doar nume sigure ajung la fisiere / mesaje: fara ':' si fara caractere de control */
    for(const char *p = user; *p; p++){
        if(*p == ':' || *p == '/' || (unsigned char)*p < ' '){
            fprintf(stderr, "login: nume de utilizator invalid\n");
            return 1;
        }
    }
    struct passwd *pw = getpwnam(user);
    char name[64];
    snprintf(name, sizeof(name), "%s", user);
    int known = pw != NULL;

    int ok = 0;
    int attempts = (interactive && mode != M_KEY) ? 3 : 1;
    for(int a = 0; a < attempts && !ok; a++){
        char secret[SECRET_MAX];
        size_t len = 0;
        if(mode != M_KEY){
            int n = adb_read_line(mode == M_PIN ? "PIN: " : "Parola: ", 0, secret, sizeof(secret));
            if(n == -1){
                break;              /* EOF */
            }
            len = n < 0 ? 0 : (size_t)n;    /* prea lunga: tratata ca gresita (len 0) */
        }
        ok = check(name, mode, secret, len, key_override) && known;
        crypto_wipe(secret, sizeof(secret));
        if(!ok){
            fprintf(stderr, "login: autentificare esuata\n");
            sleep(1);
        }
    }
    if(!ok){
        return 1;
    }
    return start_session(name, cmd);
}

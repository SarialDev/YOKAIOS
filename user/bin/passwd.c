/* passwd [-p | -k FISIER] [-s SHADOW] [-a AUTH] [-M PREFIX] [utilizator]
 * Schimba parola (implicit), PIN-ul (-p) sau inroleaza un fisier-cheie (-k, calea trebuie sa fie sub /media/) pentru login.
 * Hash-ul se scrie ca $pbkdf2-sha256$100000$sare(16 octeti din /dev/random)$hash in /etc/shadow, respectiv /etc/auth
 * (fisier temporar + rename). root poate schimba orice cont, fara parola veche; ceilalti doar contul lor, cu parola curenta,
 * si programul trebuie sa fie setuid root. Cu radacina read-only (initrd) scrierea in /etc esueaza (EROFS).
 * Parola: cel putin 6 caractere; PIN: 4-16 cifre; fisier-cheie: 1-4096 octeti. -s -a -M: doar root, pentru teste. */
#include "authdb.h"
#include <pwd.h>

static const char *shadow_file = SHADOW_FILE;
static const char *auth_file = AUTH_FILE;
static const char *key_prefix = KEY_PREFIX;

static void usage(void){
    fprintf(stderr, "uz: passwd [-p | -k fisier] [utilizator]\n");
}

static int valid_pin(const char *s){
    size_t n = strlen(s);
    if(n < 4 || n > 16){
        return 0;
    }
    for(; *s; s++){
        if(*s < '0' || *s > '9'){
            return 0;
        }
    }
    return 1;
}

int main(int argc, char **argv){
    int pin = 0;
    const char *keyfile = NULL, *target = NULL;
    for(int i = 1; i < argc; i++){
        if(strcmp(argv[i], "--help") == 0){
            puts("uz: passwd [-p | -k fisier] [utilizator]");
            return 0;
        } else if(strcmp(argv[i], "-p") == 0){
            pin = 1;
        } else if(strcmp(argv[i], "-k") == 0 && i + 1 < argc){
            keyfile = argv[++i];
        } else if((strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "-M") == 0) && i + 1 < argc){
            if(getuid() != 0){
                fprintf(stderr, "passwd: %s: doar root\n", argv[i]);
                return 1;
            }
            const char *v = argv[i + 1];
            if(argv[i][1] == 's'){
                shadow_file = v;
            } else if(argv[i][1] == 'a'){
                auth_file = v;
            } else {
                key_prefix = v;
            }
            i++;
        } else if(argv[i][0] != '-' && !target){
            target = argv[i];
        } else {
            usage();
            return 2;
        }
    }
    if(pin && keyfile){
        usage();
        return 2;
    }
    if(geteuid() != 0){
        fprintf(stderr, "passwd: nu am drepturi de scriere (programul trebuie setuid root)\n");
        return 1;
    }
    struct passwd *me = getpwuid(getuid());
    char myname[64] = "";
    if(me){
        snprintf(myname, sizeof(myname), "%s", me->pw_name);
    }
    char user[64];
    snprintf(user, sizeof(user), "%s", target ? target : myname);
    if(!*user || !getpwnam(user)){
        fprintf(stderr, "passwd: utilizator necunoscut: %s\n", user);
        return 1;
    }
    if(getuid() != 0 && strcmp(user, myname) != 0){
        fprintf(stderr, "passwd: doar root poate schimba contul altcuiva\n");
        return 1;
    }

    /* Cine nu e root isi dovedeste intai parola curenta */
    if(getuid() != 0){
        char cur[SECRET_MAX], hash[PWHASH_MAX + 16];
        int n = adb_read_line("Parola curenta: ", 0, cur, sizeof(cur));
        int ok = 0;
        if(n >= 0 && adb_lookup(shadow_file, user, NULL, hash, sizeof(hash), NULL, 0) == 1 && pwhash_valid(hash)){
            ok = pwhash_verify(cur, (size_t)n, hash) == 1;
        }
        crypto_wipe(cur, sizeof(cur));
        if(!ok){
            sleep(1);
            fprintf(stderr, "passwd: parola curenta gresita\n");
            return 1;
        }
    }

    char hash[PWHASH_MAX];
    if(keyfile){
        static char key[KEY_MAX + 1];
        if(!adb_key_path_ok(keyfile, key_prefix)){
            fprintf(stderr, "passwd: fisierul-cheie trebuie sa fie sub %s\n", key_prefix);
            return 1;
        }
        long n = adb_read_key(keyfile, key);
        if(n < 0){
            fprintf(stderr, "passwd: %s: nu pot citi (gol, mai mare de %d octeti sau lipsa)\n", keyfile, KEY_MAX);
            return 1;
        }
        int rc = adb_hash_new(key, (size_t)n, hash);
        crypto_wipe(key, sizeof(key));
        if(rc < 0){
            fprintf(stderr, "passwd: nu pot genera hash-ul\n");
            return 1;
        }
        if(adb_store(auth_file, user, "key", keyfile, hash) < 0){
            fprintf(stderr, "passwd: %s: %s\n", auth_file, strerror(errno));
            return 1;
        }
        printf("fisier-cheie inrolat pentru %s\n", user);
        return 0;
    }

    char a[SECRET_MAX], b[SECRET_MAX];
    int la = adb_read_line(pin ? "PIN nou: " : "Parola noua: ", 0, a, sizeof(a));
    int lb = la >= 0 ? adb_read_line(pin ? "Reintroduceti PIN-ul: " : "Reintroduceti parola: ", 0, b, sizeof(b)) : -1;
    int rc = 1;
    if(la < 0 || lb < 0){
        fprintf(stderr, "passwd: intrare incompleta sau prea lunga\n");
    } else if(la != lb || strcmp(a, b) != 0){
        fprintf(stderr, "passwd: cele doua valori nu coincid\n");
    } else if(pin ? !valid_pin(a) : la < 6){
        fprintf(stderr, pin ? "passwd: PIN-ul are 4-16 cifre\n" : "passwd: parola are cel putin 6 caractere\n");
    } else if(adb_hash_new(a, (size_t)la, hash) < 0){
        fprintf(stderr, "passwd: nu pot genera hash-ul\n");
    } else if((pin ? adb_store(auth_file, user, "pin", NULL, hash) : adb_store(shadow_file, user, NULL, NULL, hash)) < 0){
        fprintf(stderr, "passwd: %s: %s\n", pin ? auth_file : shadow_file, strerror(errno));
    } else {
        printf(pin ? "PIN schimbat pentru %s\n" : "parola schimbata pentru %s\n", user);
        rc = 0;
    }
    crypto_wipe(a, sizeof(a));
    crypto_wipe(b, sizeof(b));
    return rc;
}

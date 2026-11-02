/* su [-] [utilizator] [-c comanda]: porneste shell-ul utilizatorului (implicit root) cu uid/gid/grupurile lui.
 * Nu exista inca parole (login vine mai tarziu): doar root poate folosi su. "-" = mediu de login (cd in HOME). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: su [-] [utilizator] [-c comanda]");
        return 0;
    }
    int login = 0;
    const char *user = "root";
    const char *cmd = NULL;
    for(int i = 1; i < argc; i++){
        if(strcmp(argv[i], "-") == 0 || strcmp(argv[i], "-l") == 0){
            login = 1;
        } else if(strcmp(argv[i], "-c") == 0 && i + 1 < argc){
            cmd = argv[++i];
        } else if(argv[i][0] != '-'){
            user = argv[i];
        } else {
            fprintf(stderr, "uz: su [-] [utilizator] [-c comanda]\n");
            return 2;
        }
    }
    struct passwd *pw = getpwnam(user);
    if(!pw){
        fprintf(stderr, "su: utilizator necunoscut: %s\n", user);
        return 1;
    }
    if(getuid() != 0){
        fprintf(stderr, "su: doar root poate schimba utilizatorul (nu exista inca parole)\n");
        return 1;
    }
    // Copii: pw arata in bufferul static, pe care initgroups il refoloseste
    char name[64], home[128], shell[128];
    snprintf(name, sizeof(name), "%s", pw->pw_name);
    snprintf(home, sizeof(home), "%s", pw->pw_dir);
    snprintf(shell, sizeof(shell), "%s", *pw->pw_shell ? pw->pw_shell : "/bin/ysh");
    uid_t uid = pw->pw_uid;
    gid_t gid = pw->pw_gid;
    if(initgroups(name, gid) < 0 || setgid(gid) < 0 || setuid(uid) < 0){
        fprintf(stderr, "su: %s\n", strerror(errno));
        return 1;
    }
    setenv("USER", name, 1);
    setenv("LOGNAME", name, 1);
    setenv("HOME", home, 1);
    setenv("SHELL", shell, 1);
    if(login && chdir(home) < 0){
        fprintf(stderr, "su: %s: %s\n", home, strerror(errno));
    }
    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;
    if(cmd){
        char *av[] = {(char *)base, "-c", (char *)cmd, NULL};
        execv(shell, av);
    } else {
        char *av[] = {(char *)base, login ? "-l" : NULL, NULL};
        execv(shell, av);
    }
    fprintf(stderr, "su: %s: %s\n", shell, strerror(errno));
    return 127;
}

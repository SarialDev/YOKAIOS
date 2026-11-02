/* whoami: numele utilizatorului efectiv (din /etc/passwd), altfel uid-ul */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: whoami");
        return 0;
    }
    if(argc > 1){
        fprintf(stderr, "uz: whoami\n");
        return 2;
    }
    uid_t u = geteuid();
    struct passwd *pw = getpwuid(u);
    if(pw){
        puts(pw->pw_name);
    } else {
        printf("%u\n", u);
    }
    return 0;
}

/* Primul program C cu libc-ul propriu: argv, envp, printf, malloc, exit. Rulat din /bin/hello (test=libc) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv, char **envp){
    printf("hello from libc: argc=%d argv[0]=%s\n", argc, argv[0]);
    for(int i = 1; i < argc; i++){
        printf("  arg %d = \"%s\"\n", i, argv[i]);
    }
    int env = 0;
    while(envp[env]){
        env++;
    }
    printf("  env=%d YOKAI=%s pid>0=%d\n", env, getenv("YOKAI") ? getenv("YOKAI") : "(null)", getpid() > 0);
    char *buf = malloc(100);
    if(!buf){
        fputs("hello: malloc a esuat\n", stderr);
        return 1;
    }
    snprintf(buf, 100, "%5d|%-5d|%05d|%x|%#x|%c|%s|%.2s|%ld|%lu", 42, 42, 42, 255, 255, 'Z', "str", "abcdef", -1234567890123L, 18446744073709551615UL);
    puts(buf);
    free(buf);
    return argc == 3 ? 0 : 2;
}

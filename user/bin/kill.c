/* kill [-SEMNAL | -s SEMNAL] pid...: implicit TERM; semnalul poate fi numar sau nume (HUP INT KILL TERM ...) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

static const struct { const char *name; int num; } sigs[] = {
    {"HUP", SIGHUP}, {"INT", SIGINT}, {"QUIT", SIGQUIT}, {"ILL", SIGILL}, {"ABRT", SIGABRT}, {"FPE", SIGFPE},
    {"KILL", SIGKILL}, {"USR1", SIGUSR1}, {"SEGV", SIGSEGV}, {"USR2", SIGUSR2}, {"PIPE", SIGPIPE},
    {"ALRM", SIGALRM}, {"TERM", SIGTERM},
};

static int parse_signal(const char *s){
    if(s[0] >= '0' && s[0] <= '9'){
        char *end;
        long n = strtol(s, &end, 10);
        return *end || n < 0 || n >= NSIG ? -1 : (int)n;
    }
    if(strncmp(s, "SIG", 3) == 0){
        s += 3;
    }
    for(unsigned i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++){
        if(strcmp(s, sigs[i].name) == 0){
            return sigs[i].num;
        }
    }
    return -1;
}

int main(int argc, char **argv){
    if(argc == 2 && strcmp(argv[1], "--help") == 0){
        puts("uz: kill [-SEMNAL | -s SEMNAL] pid...");
        return 0;
    }
    int sig = SIGTERM, i = 1;
    if(i < argc && strcmp(argv[i], "-s") == 0 && i + 1 < argc){
        sig = parse_signal(argv[i + 1]);
        i += 2;
    } else if(i < argc && argv[i][0] == '-' && argv[i][1]){
        sig = parse_signal(argv[i] + 1);
        i++;
    }
    if(sig < 0){
        fprintf(stderr, "kill: semnal necunoscut\n");
        return 2;
    }
    if(i >= argc){
        fprintf(stderr, "uz: kill [-SEMNAL | -s SEMNAL] pid...\n");
        return 2;
    }
    int rc = 0;
    for(; i < argc; i++){
        char *end;
        long pid = strtol(argv[i], &end, 10);
        if(!*argv[i] || *end || pid <= 0){
            fprintf(stderr, "kill: pid invalid: %s\n", argv[i]);
            rc = 1;
            continue;
        }
        if(kill((pid_t)pid, sig) < 0){
            fprintf(stderr, "kill: %ld: %s\n", pid, strerror(errno));
            rc = 1;
        }
    }
    return rc;
}

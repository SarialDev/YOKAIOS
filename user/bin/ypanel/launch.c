/* ypanel: pornirea aplicatiilor. Comanda simpla (cai + argumente separate prin spatii) se executa direct; cu metacaractere de
 * shell (| & ; < > $ ghilimele) trece prin ysh -c. Copilul nu pastreaza conexiunea la compozitor. */
#include "panel.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

int app_launch(const char *cmd){
    char buf[CMD_LEN];
    snprintf(buf, sizeof(buf), "%s", cmd);
    int shell = strpbrk(cmd, "|&;<>$\"'`*?") != NULL;
    pid_t pid = fork();
    if(pid < 0){
        return -1;
    }
    if(pid == 0){
        close(yw_fd(P.c));
        if(!shell){
            char *av[16];
            int n = 0;
            for(char *p = strtok(buf, " \t"); p && n < 15; p = strtok(NULL, " \t")){
                av[n++] = p;
            }
            av[n] = NULL;
            if(n){
                char path[CMD_LEN + 8];
                if(strchr(av[0], '/')){
                    snprintf(path, sizeof(path), "%s", av[0]);
                } else {
                    snprintf(path, sizeof(path), "/bin/%s", av[0]);
                }
                execv(path, av);
            }
        }
        char *sh[] = {"ysh", "-c", buf, NULL};
        execv("/bin/ysh", sh);
        _exit(127);
    }
    P.launches++;
    return 0;
}

void reap_children(void){
    while(waitpid(-1, NULL, WNOHANG) > 0){
    }
}

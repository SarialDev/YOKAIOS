#ifndef _SYS_UN_H
#define _SYS_UN_H
#include <sys/socket.h>
/* AF_UNIX (T10.2): SOCK_STREAM. Numele (sun_path, sau abstract cu '\0' la inceput) stau in kernel, nu in sistemul de
 * fisiere: nu se creeaza niciun fisier si numele se elibereaza la close (nu e nevoie de unlink inainte de bind). */
struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[108];
};
#endif

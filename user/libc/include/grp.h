#ifndef _GRP_H
#define _GRP_H
#include <sys/types.h>
/* /etc/group: nume:x:gid:membru1,membru2. Rezultat static, ca la pwd.h */
struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};
struct group *getgrnam(const char *name);
struct group *getgrgid(gid_t gid);
/* Parcurgere: setgrent (de la inceput), getgrent (urmatorul, NULL la sfarsit), endgrent */
void setgrent(void);
struct group *getgrent(void);
void endgrent(void);
/* Grupurile suplimentare (doar root): setgroups direct, initgroups = gid + grupurile din /etc/group care il listeaza pe user */
int setgroups(size_t size, const gid_t *list);
int initgroups(const char *user, gid_t gid);
#endif

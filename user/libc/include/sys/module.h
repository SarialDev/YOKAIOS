#ifndef _SYS_MODULE_H
#define _SYS_MODULE_H
/* Module de kernel semnate (kernel/core/kmod.h); ambele cer CAP_SYS_ADMIN.
 * init_module: imaginea = ELF relocabil + semnatura Ed25519 + magic (tools/sign.py sign-module); errno ENOKEY = nesemnat,
 * EKEYREJECTED = semnatura gresita, ENOEXEC = ELF invalid / relocare nesuportata, ENOENT = simbol nerezolvat */
int init_module(const void *image, unsigned long len);
int delete_module(const char *name);
#endif

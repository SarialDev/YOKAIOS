#ifndef _SYS_SIGN_H
#define _SYS_SIGN_H
/* Semnaturi Ed25519 verificate de kernel cu cheia lui publica (kernel/core/verify.h).
 * sigverify(SIGV_PACKAGE, manifest_blanc, lungime, sig64) = 0 sau -1 cu errno EKEYREJECTED (semnatura gresita);
 * sigverify(SIGV_POLICY, 0, 0, 0) = 1 daca kernelul are sign=permissive (ypkg -U poate instala pachete nesemnate) */
#define SIGV_PACKAGE 0
#define SIGV_POLICY  1
long sigverify(int op, const void *data, unsigned long len, const void *sig);
#endif

#ifndef _SETJMP_H
#define _SETJMP_H
typedef long jmp_buf[8];
int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int val) __attribute__((noreturn));
int _setjmp(jmp_buf env) __attribute__((returns_twice));
void _longjmp(jmp_buf env, int val) __attribute__((noreturn));
#define setjmp(env) setjmp(env)
#endif

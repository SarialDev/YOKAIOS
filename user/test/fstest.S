/* Syscall-urile de fisiere din ring 3 (open, read, write, lseek, stat, fstat, mkdir, rmdir, unlink, symlink, readlink,
 * getdents64, chdir, getcwd, mount, umount2, fork cu cwd mostenit). Presupune un memfs montat la "/" cu directorul
 * "/a" (le pregateste kernel/tests/test_vfs.cpp). Iese cu 0 sau cu numarul verificarii esuate. */
#include "uapi/syscall_nr.h"
#include "uapi/fs.h"

.macro sc nr
    movl $\nr, %eax
    syscall
.endm

/* rax trebuie sa fie val, altfel exit(code) */
.macro expect val, code
    cmpq $\val, %rax
    je 1f
    movl $\code, %edi
    jmp fail
1:
.endm

.macro path reg, sym
    leaq \sym(%rip), \reg
.endm

.section .text
.globl _start
_start:
    path %rdi, p_u
    movl $0755, %esi
    sc SYS_MKDIR
    expect 0, 1
    path %rdi, p_u
    movl $0755, %esi
    sc SYS_MKDIR
    expect -EEXIST, 2

    path %rdi, p_uf
    movl $(O_CREAT | O_RDWR), %esi
    movl $0644, %edx
    sc SYS_OPEN
    expect 3, 3                         /* primul fd liber dupa stdin/stdout/stderr */
    movq %rax, %r12
    movq %r12, %rdi
    path %rsi, msg
    movl $5, %edx
    sc SYS_WRITE
    expect 5, 4
    movq %r12, %rdi
    xorl %esi, %esi
    movl $SEEK_SET, %edx
    sc SYS_LSEEK
    expect 0, 5
    movq %r12, %rdi
    path %rsi, buf
    movl $16, %edx
    sc SYS_READ
    expect 5, 6
    cmpb $'h', buf(%rip)
    movl $7, %edi
    jne fail
    cmpb $'o', buf+4(%rip)
    movl $7, %edi
    jne fail

    movq %r12, %rdi
    path %rsi, st
    sc SYS_FSTAT
    expect 0, 8
    cmpq $5, st+48(%rip)                /* st_size */
    movl $9, %edi
    jne fail
    movl st+24(%rip), %eax              /* st_mode */
    andl $S_IFMT, %eax
    cmpl $S_IFREG, %eax
    movl $10, %edi
    jne fail
    path %rdi, p_uf
    path %rsi, st
    sc SYS_STAT
    expect 0, 11
    cmpq $5, st+48(%rip)
    movl $11, %edi
    jne fail
    path %rdi, p_none
    path %rsi, st
    sc SYS_STAT
    expect -ENOENT, 12

    path %rdi, p_u
    sc SYS_CHDIR
    expect 0, 13
    path %rdi, buf
    movl $64, %esi
    sc SYS_GETCWD
    expect 3, 14                        /* "/u" + terminator */
    cmpb $'u', buf+1(%rip)
    movl $14, %edi
    jne fail
    path %rdi, buf
    movl $2, %esi
    sc SYS_GETCWD
    expect -ERANGE, 15
    path %rdi, p_rel
    xorl %esi, %esi
    sc SYS_OPEN                         /* "f" relativ la cwd */
    expect 4, 16
    movq %rax, %rdi
    sc SYS_CLOSE
    expect 0, 17

    path %rdi, p_dot
    movl $(O_RDONLY | O_DIRECTORY), %esi
    sc SYS_OPEN
    expect 4, 18
    movq %rax, %r13
    movq %r13, %rdi
    path %rsi, buf
    movl $512, %edx
    sc SYS_GETDENTS64
    testq %rax, %rax
    movl $19, %edi
    jle fail
    cmpq $72, %rax                      /* ".", ".." si "f": 3 x 24 */
    movl $19, %edi
    jne fail
    movq %r13, %rdi
    path %rsi, buf
    movl $512, %edx
    sc SYS_GETDENTS64
    expect 0, 20
    movq %r13, %rdi
    sc SYS_CLOSE
    expect 0, 21

    path %rdi, p_uf
    path %rsi, p_ul
    sc SYS_SYMLINK
    expect 0, 22
    path %rdi, p_ul
    path %rsi, buf
    movl $64, %edx
    sc SYS_READLINK
    expect 4, 23                        /* "/u/f" */
    path %rdi, p_ul
    xorl %esi, %esi
    sc SYS_OPEN                         /* prin symlink */
    expect 4, 24
    movq %rax, %rdi
    sc SYS_CLOSE
    path %rdi, p_ul
    sc SYS_UNLINK
    expect 0, 25
    movq %r12, %rdi
    sc SYS_CLOSE
    expect 0, 26
    movq %r12, %rdi
    sc SYS_CLOSE
    expect -EBADF, 26
    path %rdi, p_uf
    sc SYS_UNLINK
    expect 0, 27
    path %rdi, p_root
    sc SYS_CHDIR
    expect 0, 28
    path %rdi, p_u
    sc SYS_RMDIR
    expect 0, 29
    path %rdi, p_uf
    xorl %esi, %esi
    sc SYS_OPEN
    expect -ENOENT, 30

    movl $1, %edi                       /* stdout: fara inode */
    xorl %esi, %esi
    movl $SEEK_SET, %edx
    sc SYS_LSEEK
    expect -ESPIPE, 31
    movl $1, %edi
    path %rsi, st
    sc SYS_FSTAT
    expect 0, 32
    movl st+24(%rip), %eax
    andl $S_IFMT, %eax
    cmpl $S_IFCHR, %eax
    movl $32, %edi
    jne fail

    path %rdi, p_um
    movl $0755, %esi
    sc SYS_MKDIR
    expect 0, 33
    path %rdi, p_none
    path %rsi, p_um
    path %rdx, p_memfs
    xorl %r10d, %r10d
    xorl %r8d, %r8d
    sc SYS_MOUNT
    expect 0, 34
    path %rdi, p_um
    xorl %esi, %esi
    sc SYS_UMOUNT2
    expect 0, 35
    path %rdi, p_um
    sc SYS_RMDIR
    expect 0, 36

    /* cwd mostenit prin fork: copilul vede "/a" */
    path %rdi, p_a
    sc SYS_CHDIR
    expect 0, 37
    sc SYS_FORK
    testq %rax, %rax
    jz child
    movq %rax, %rdi
    path %rsi, status
    xorl %edx, %edx
    sc SYS_WAIT4
    testq %rax, %rax
    movl $38, %edi
    jle fail
    cmpl $0, status(%rip)
    movl $39, %edi
    jne fail
    xorl %edi, %edi
    jmp fail

child:
    path %rdi, buf
    movl $64, %esi
    sc SYS_GETCWD
    expect 3, 40
    cmpb $'a', buf+1(%rip)
    movl $41, %edi
    jne fail
    path %rdi, p_root
    sc SYS_CHDIR                        /* cwd-ul copilului nu schimba cwd-ul parintelui */
    xorl %edi, %edi
    jmp fail

fail:                                   /* exit(edi); 0 = reusit */
    sc SYS_EXIT
    hlt

.section .rodata
p_u:    .asciz "/u"
p_uf:   .asciz "/u/f"
p_ul:   .asciz "/u/l"
p_rel:  .asciz "f"
p_dot:  .asciz "."
p_root: .asciz "/"
p_a:    .asciz "/a"
p_um:   .asciz "/um"
p_none: .asciz "none"
p_memfs: .asciz "memfs"
msg:    .ascii "hello"

.section .bss
.align 8
buf:    .skip 512
st:     .skip 144
status: .skip 8

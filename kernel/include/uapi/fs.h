#pragma once
/* Constante si structuri de fisiere, comune pentru kernel si programele user (numerele sunt cele de pe Linux x86-64) */

#ifndef __ASSEMBLER__
#include "types.h"
#endif

#define O_RDONLY        0
#define O_WRONLY        1
#define O_RDWR          2
#define O_ACCMODE       3
#define O_CREAT         0100
#define O_EXCL          0200
#define O_TRUNC         01000
#define O_APPEND        02000
#define O_NONBLOCK      04000
#define O_DIRECTORY     0200000
#define O_NOFOLLOW      0400000

#define SEEK_SET        0
#define SEEK_CUR        1
#define SEEK_END        2

#define S_IFMT          0170000
#define S_IFREG         0100000
#define S_IFDIR         0040000
#define S_IFLNK         0120000
#define S_IFCHR         0020000
#define S_IFBLK         0060000
#define S_IFIFO         0010000
#define S_ISREG(m)      (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m)      (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m)      (((m) & S_IFMT) == S_IFLNK)
#define S_ISCHR(m)      (((m) & S_IFMT) == S_IFCHR)
#define S_ISBLK(m)      (((m) & S_IFMT) == S_IFBLK)

#define DT_UNKNOWN      0
#define DT_FIFO         1
#define DT_CHR          2
#define DT_DIR          4
#define DT_BLK          6
#define DT_REG          8
#define DT_LNK          10

#define MS_RDONLY       1

#define PATH_MAX        256
#define NAME_MAX        255

#ifndef __ASSEMBLER__
struct Timespec {
    int64_t sec;
    int64_t nsec;
};

// struct stat de pe Linux x86-64 (144 bytes)
struct Stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    Timespec st_atim;
    Timespec st_mtim;
    Timespec st_ctim;
    int64_t unused[3];
};
static_assert(sizeof(Stat) == 144, "Stat ca pe Linux x86-64");

// linux_dirent64: reclen multiplu de 8, name terminat cu 0
struct Dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};
#endif

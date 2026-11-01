#ifndef _DIRENT_H
#define _DIRENT_H
#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
struct dirent {
    unsigned long d_ino;
    unsigned char d_type;
    char d_name[256];
};
typedef struct _DIR DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *d);
int closedir(DIR *d);
#endif

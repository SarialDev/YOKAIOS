#pragma once
#include "types.h"

// Partea de VFS a netcfg, separata de netcfg.cpp (antetele lwIP si uapi/fs.h isi redefinesc PATH_MAX)
// Creeaza / trunchiaza path si scrie data; 0 sau -errno
int netcfg_fs_write(const char* path, const char* data, uint64_t len);
// true daca path exista si e director (de ex. /run dupa montarea tmpfs)
bool netcfg_fs_dir_exists(const char* path);

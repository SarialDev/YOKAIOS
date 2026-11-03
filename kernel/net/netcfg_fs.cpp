#include "net/netcfg_fs.h"
#include "uapi/fs.h"
#include "fs/vfs/vfs.h"

int netcfg_fs_write(const char* path, const char* data, uint64_t len){
    const Path none = {};
    File* f;
    int err = vfs_open(none, path, O_WRONLY | O_CREAT | O_TRUNC, 0644, f);
    if(err){
        return err;
    }
    int64_t n = f->ops->write(f, data, len, false);
    file_put(f);
    return n == (int64_t)len ? 0 : (n < 0 ? (int)n : -5);
}

bool netcfg_fs_dir_exists(const char* path){
    const Path none = {};
    Stat st;
    return vfs_stat(none, path, true, st) == 0 && S_ISDIR(st.st_mode);
}

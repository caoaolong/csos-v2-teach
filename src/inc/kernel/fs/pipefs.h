#ifndef CSOS_FS_PIPEFS_H
#define CSOS_FS_PIPEFS_H

#include <kernel/fs/vfs.h>

extern const fs_driver_t pipefs_driver;

void init_pipefs();

#endif /* CSOS_FS_PIPEFS_H */

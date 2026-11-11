#ifndef CSOS_FS_DEVFS_H
#define CSOS_FS_DEVFS_H

#include <fs/vfs.h>

extern const fs_driver_t devfs_driver;

void init_devfs();

#endif /* CSOS_FS_DEVFS_H */

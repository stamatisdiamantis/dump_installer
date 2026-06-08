#ifndef UFS_MOUNT_H
#define UFS_MOUNT_H

#include <stdbool.h>

bool mount_ufs_image(const char *file_path, char *out_mount_point);
bool mount_ufs_image_ex(const char *file_path, char *out_mount_point, bool skip_freshness_check);
void unmount_ufs(const char *mount_point);

#endif

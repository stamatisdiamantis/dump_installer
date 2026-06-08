#ifndef EXFAT_MOUNT_H
#define EXFAT_MOUNT_H

#include "types.h"

#include <stdbool.h>
#include <sys/stat.h>

bool mount_exfat_image(const char *file_path, char *out_mount_point);
bool mount_exfat_image_ex(const char *file_path, char *out_mount_point, bool skip_freshness_check);
bool mount_exfat_image_at(const char *file_path, const char *mount_point, bool skip_freshness_check);
bool mount_exfat_image_at_rw(const char *file_path, const char *mount_point,
                             bool skip_freshness_check);
void unmount_exfat(const char *mount_point);
void unmount_exfat_mount_point(const char *mount_point);
bool detach_exfat_md_mount(const char *mount_point);

#endif

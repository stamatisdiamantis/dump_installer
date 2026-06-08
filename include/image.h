#ifndef IMAGE_H
#define IMAGE_H

#include "types.h"

bool is_pfsc_image(const char *name);
bool is_pfs_image(const char *name);
bool is_ffpfs_image(const char *name);
bool is_ufs_image(const char *name);
bool is_exfat_image(const char *name);
bool is_nested_pfs_name(const char *name);
bool is_pfsc_mount_path(const char *path);

image_type_t detect_image_type_for_path(const char *path, const char *name);

int list_images_in_dir(const char *dir, dir_image_t *out, int max_out);

#endif

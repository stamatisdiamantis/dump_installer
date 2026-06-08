#ifndef LVD_MOUNT_H
#define LVD_MOUNT_H

#include <stdbool.h>
#include "types.h"

bool lvd_attach_pfs(const char *file_path, uint8_t nested_selector, uint32_t sector_size,
                    int *unit_id_out, char *devname_out, size_t devname_size);

void lvd_detach(int unit_id);

bool nmount_lvd_pfs(const char *devname, const char *mount_point,
                    uint8_t nested_selector);

bool nmount_lvd_pfs_ex(const char *devname, const char *mount_point,
                       uint8_t nested_selector, bool force);

bool mount_lvd_pfs_image(const char *file_path, image_type_t fs_type,
                         const char *mount_base, char *out_mount_point,
                         int *unit_id_out);

bool mount_lvd_pfs_image_at(const char *file_path, image_type_t fs_type,
                            const char *mount_point, int *unit_id_out);

bool mount_lvd_pfs_region(const char *container_path, uint64_t offset, uint64_t size,
                          const char *mount_base, char *out_mount_point, int *unit_id_out);

void unmount_lvd_pfs(const char *mount_point, int unit_id);

bool relocate_lvd_pfs_mount(int unit_id, uint8_t nested_selector,
                            const char *old_mount, const char *new_mount);

void build_image_mount_point(const char *file_path, image_type_t fs_type,
                             const char *mount_base, char mount_point[MAX_PATH]);

#endif

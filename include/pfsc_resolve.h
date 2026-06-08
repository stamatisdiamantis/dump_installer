#ifndef PFSC_RESOLVE_H
#define PFSC_RESOLVE_H

#include <stdbool.h>
#include "types.h"

typedef struct {
    char outer_mount[MAX_PATH];
    char inner_mount[MAX_PATH];
    char inner_image_path[MAX_PATH];
    char outer_image_path[MAX_PATH];
    image_type_t inner_type;
    int lvd_outer_unit;
    int lvd_inner_unit;
    bool inner_is_lvd_pfs;
    bool inner_is_save_pfs;
    bool inner_is_ufs;
    bool inner_is_exfat;
} pfsc_mount_result_t;

void pfsc_mount_result_init(pfsc_mount_result_t *result);

bool mount_pfsc_container(const char *file_path, pfsc_mount_result_t *result);

bool resolve_nested_game_image(pfsc_mount_result_t *result);

void unmount_pfsc_result(pfsc_mount_result_t *result);

bool remount_pfsc_inner_pfs_from_outer_offset(pfsc_mount_result_t *result);

bool remount_pfsc_inner_pfs_via_save_data(pfsc_mount_result_t *result, const char *stage_dir);

#endif

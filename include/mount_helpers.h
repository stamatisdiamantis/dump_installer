#ifndef MOUNT_HELPERS_H

#define MOUNT_HELPERS_H



#include <stdbool.h>



int remount_system_ex(void);

int mount_nullfs(const char *src, const char *dst);

bool mount_title_nullfs(const char *title_id, const char *src_path);

bool mount_overlay_to_system_ex(const char *src, const char *dst);

bool mount_overlay_via_bridge(const char *src, const char *dst, const char *bridge);

int prepare_system_ex_for_nullfs(const char *title_id);

int prepare_system_ex_title_reinstall(const char *title_id);

int is_mounted(const char *path);

int prepare_system_ex_mount_point(const char *title_id);

bool mount_fstype_at_path(const char *fstype, const char *path);

void cleanup_legacy_dilocs(void);

void recover_system_ex_overlay_mounts(void);

void reset_installer_session_mounts(void);

#endif


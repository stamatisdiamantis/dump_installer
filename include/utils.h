#ifndef UTILS_H
#define UTILS_H

#include "types.h"

#include <stdbool.h>

void notify(const char *fmt, ...);
void browser_log(const char *fmt, ...);
bool notify_game_installed(const char *title_id);
bool notify_installer_toast(const char *message);
int copy_file(const char *src, const char *dst);
int copy_dir(const char *src, const char *dst);
int remove_dir(const char *path);
int is_appmeta_file(const char *name);
bool find_subfile_offset_in_container(const char *container_path, const char *inner_path,
                                      uint64_t inner_size, uint64_t *offset_out);
bool read_pfsc_inner_offset_cache(const char *container_path, uint64_t inner_size,
                                  uint64_t *offset_out);
void write_pfsc_inner_offset_cache(const char *container_path, uint64_t inner_size,
                                   uint64_t offset);

int sceKernelSendNotificationRequest(int, notify_request_t*, size_t, int);

#endif
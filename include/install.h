#ifndef INSTALL_H
#define INSTALL_H

#include <stddef.h>

int get_title_id(const char *base_path, char *title_id, size_t size);
int copy_sce_sys_to_appmeta(const char *src, const char *title_id);
int update_trophy(const char *title_id, const char *src_sce_sys);
int update_snd0info(const char *title_id);

#endif
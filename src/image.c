#include "image.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <strings.h>

bool is_pfsc_image(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".ffpfsc");
}

bool is_ffpfs_image(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".ffpfs");
}

bool is_pfs_image(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".pfs") || is_ffpfs_image(name);
}

bool is_ufs_image(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".ffpkg") || !strcasecmp(dot, ".ufs");
}

bool is_exfat_image(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return !strcasecmp(dot, ".exfat") || !strcasecmp(dot, ".img");
}

bool is_nested_pfs_name(const char *name) {
    return name && strcasecmp(name, "pfs_image.dat") == 0;
}

bool is_pfsc_mount_path(const char *path) {
    if (!path) return false;
    size_t len = strlen(PFSC_IMAGE_MOUNT_BASE);
    if (strncmp(path, PFSC_IMAGE_MOUNT_BASE, len) != 0)
        return false;
    return path[len] == '\0' || path[len] == '/';
}

image_type_t detect_image_type_for_path(const char *path, const char *name) {
    const char *filename = name;
    if ((!filename || filename[0] == '\0') && path) {
        const char *base = strrchr(path, '/');
        filename = base ? base + 1 : path;
    }

    if (is_pfsc_image(filename))
        return IMAGE_TYPE_PFSC;
    if (is_pfs_image(filename))
        return IMAGE_TYPE_PFS;
    if (is_ufs_image(filename))
        return IMAGE_TYPE_UFS;
    if (is_exfat_image(filename))
        return IMAGE_TYPE_EXFAT;
    if (is_nested_pfs_name(filename) && is_pfsc_mount_path(path))
        return IMAGE_TYPE_PFS;

    return IMAGE_TYPE_UNKNOWN;
}

int list_images_in_dir(const char *dir, dir_image_t *out, int max_out) {
    DIR *d = opendir(dir);
    if (!d || !out || max_out <= 0)
        return 0;

    int count = 0;
    struct dirent *e;

    while ((e = readdir(d)) && count < max_out) {
        if (e->d_name[0] == '.')
            continue;

        image_type_t type = detect_image_type_for_path(NULL, e->d_name);
        if (type == IMAGE_TYPE_UNKNOWN)
            continue;

        snprintf(out[count].path, sizeof(out[count].path), "%s/%s", dir, e->d_name);
        out[count].type = type;
        count++;
    }

    closedir(d);
    return count;
}

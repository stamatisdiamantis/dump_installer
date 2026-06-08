#include "pfsc_resolve.h"
#include "image.h"
#include "hash.h"
#include "lvd_mount.h"
#include "exfat_mount.h"
#include "ufs_mount.h"
#include "pfs_mount.h"
#include "log.h"
#include "utils.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#define PFSC_SCAN_MAX_DEPTH 3

static bool path_has_sce_sys(const char *mount_point) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/sce_sys/param.json", mount_point);
    if (access(path, F_OK) == 0)
        return true;
    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", mount_point);
    return access(path, F_OK) == 0;
}

void pfsc_mount_result_init(pfsc_mount_result_t *result) {
    memset(result, 0, sizeof(*result));
    result->lvd_outer_unit = -1;
    result->lvd_inner_unit = -1;
}

bool mount_pfsc_container(const char *file_path, pfsc_mount_result_t *result) {
    pfsc_mount_result_init(result);
    snprintf(result->outer_image_path, sizeof(result->outer_image_path), "%s", file_path);

    if (!mount_lvd_pfs_image(file_path, IMAGE_TYPE_PFSC, PFSC_IMAGE_MOUNT_BASE,
                             result->outer_mount, &result->lvd_outer_unit)) {
        notify("PFSC container mount failed: %s", file_path);
        return false;
    }

    di_logf("PFSC outer mounted at %s", result->outer_mount);
    return true;
}

typedef struct {
    pfsc_mount_result_t *result;
    bool found;
    char exfat_path[MAX_PATH];
    char pfs_path[MAX_PATH];
    char ufs_path[MAX_PATH];
    unsigned int image_count;
} nested_scan_ctx_t;

static bool try_mount_nested_candidate(const char *full_path, const char *name,
                                       nested_scan_ctx_t *ctx) {
    image_type_t type = detect_image_type_for_path(full_path, name);
    if (type == IMAGE_TYPE_UNKNOWN)
        return false;

    pfsc_mount_result_t *result = ctx->result;
    char inner_mount[MAX_PATH] = {0};
    bool mounted = false;

    if (type == IMAGE_TYPE_EXFAT) {
        if (mount_exfat_image_ex(full_path, inner_mount, true) && path_has_sce_sys(inner_mount)) {
            result->inner_is_exfat = true;
            mounted = true;
        } else if (inner_mount[0]) {
            unmount_exfat(inner_mount);
        }
    } else if (type == IMAGE_TYPE_UFS) {
        if (mount_ufs_image_ex(full_path, inner_mount, true) && path_has_sce_sys(inner_mount)) {
            result->inner_is_ufs = true;
            mounted = true;
        } else if (inner_mount[0]) {
            unmount_ufs(inner_mount);
        }
    } else if (type == IMAGE_TYPE_PFS) {
        if (mount_pfs_image(full_path, inner_mount) && path_has_sce_sys(inner_mount)) {
            result->inner_is_save_pfs = true;
            di_logf("PFSC inner PFS mounted via save-data at %s", inner_mount);
            mounted = true;
        } else if (inner_mount[0]) {
            unmount_pfs(inner_mount);
            inner_mount[0] = '\0';
        }

        if (!mounted && mount_lvd_pfs_image(full_path, IMAGE_TYPE_PFS, "/data/imgmnt/pfsmnt",
                                            inner_mount, &result->lvd_inner_unit) &&
            path_has_sce_sys(inner_mount)) {
            result->inner_is_lvd_pfs = true;
            di_logf("PFSC inner PFS mounted via nested LVD at %s", inner_mount);
            mounted = true;
        } else if (!mounted && inner_mount[0]) {
            unmount_lvd_pfs(inner_mount, result->lvd_inner_unit);
            result->lvd_inner_unit = -1;
            inner_mount[0] = '\0';
        }
    }

    if (!mounted)
        return false;

    snprintf(result->inner_mount, sizeof(result->inner_mount), "%s", inner_mount);
    snprintf(result->inner_image_path, sizeof(result->inner_image_path), "%s", full_path);
    result->inner_type = type;
    ctx->found = true;
    return true;
}

static void note_candidate(nested_scan_ctx_t *ctx, const char *full_path, const char *name) {
    image_type_t type = detect_image_type_for_path(full_path, name);
    ctx->image_count++;
    if (type == IMAGE_TYPE_EXFAT && !ctx->exfat_path[0])
        snprintf(ctx->exfat_path, sizeof(ctx->exfat_path), "%s", full_path);
    else if (type == IMAGE_TYPE_PFS && !ctx->pfs_path[0])
        snprintf(ctx->pfs_path, sizeof(ctx->pfs_path), "%s", full_path);
    else if (type == IMAGE_TYPE_UFS && !ctx->ufs_path[0])
        snprintf(ctx->ufs_path, sizeof(ctx->ufs_path), "%s", full_path);
}

static void scan_nested_dir(const char *dir_path, unsigned int depth, nested_scan_ctx_t *ctx) {
    if (ctx->found || depth > PFSC_SCAN_MAX_DEPTH)
        return;

    DIR *d = opendir(dir_path);
    if (!d)
        return;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, e->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0)
            continue;

        if (S_ISREG(st.st_mode))
            note_candidate(ctx, full_path, e->d_name);
        else if (S_ISDIR(st.st_mode))
            scan_nested_dir(full_path, depth + 1, ctx);
    }
    closedir(d);
}

static bool mount_by_priority(nested_scan_ctx_t *ctx) {
    if (ctx->exfat_path[0]) {
        const char *name = strrchr(ctx->exfat_path, '/') + 1;
        if (try_mount_nested_candidate(ctx->exfat_path, name, ctx))
            return true;
    }
    if (ctx->pfs_path[0]) {
        const char *name = strrchr(ctx->pfs_path, '/') + 1;
        if (try_mount_nested_candidate(ctx->pfs_path, name, ctx))
            return true;
    }
    if (ctx->ufs_path[0]) {
        const char *name = strrchr(ctx->ufs_path, '/') + 1;
        if (try_mount_nested_candidate(ctx->ufs_path, name, ctx))
            return true;
    }
    return false;
}

bool resolve_nested_game_image(pfsc_mount_result_t *result) {
    if (!result || !result->outer_mount[0])
        return false;

    nested_scan_ctx_t ctx = {0};
    ctx.result = result;
    di_logf("scanning nested images in %s", result->outer_mount);
    scan_nested_dir(result->outer_mount, 0, &ctx);

    if (!mount_by_priority(&ctx)) {
        if (ctx.image_count == 0) {
            notify("PFSC container has no nested image files");
        } else {
            notify("PFSC nested images found (%u) but none mounted with sce_sys",
                   ctx.image_count);
        }
        return false;
    }

    return true;
}

static void build_save_data_try_mount(const char *file_path, char *try_mount, size_t try_sz) {
    const char *filename = strrchr(file_path, '/') ? strrchr(file_path, '/') + 1 : file_path;
    char mount_name[MAX_PATH];
    char *dot;

    snprintf(mount_name, sizeof(mount_name), "%s", filename);
    dot = strrchr(mount_name, '.');
    if (dot)
        *dot = '\0';

    snprintf(try_mount, try_sz, "/data/imgmnt/pfsmnt/sd_%s_%08x",
             mount_name, fnv1a32(file_path));
}

static bool try_save_data_candidate(pfsc_mount_result_t *result, const char *file_path,
                                    char *try_mount, size_t try_sz) {
    build_save_data_try_mount(file_path, try_mount, try_sz);
    if (mount_pfs_save_data_to(file_path, try_mount) && path_has_sce_sys(try_mount))
        return true;

    unmount_pfs(try_mount);
    try_mount[0] = '\0';
    return false;
}

static void adopt_save_data_mount(pfsc_mount_result_t *result, const char *file_path,
                                  const char *try_mount) {
    char old_mount[MAX_PATH];
    int old_unit = -1;
    bool had_lvd = false;

    if (result->inner_is_lvd_pfs && result->inner_mount[0]) {
        snprintf(old_mount, sizeof(old_mount), "%s", result->inner_mount);
        old_unit = result->lvd_inner_unit;
        had_lvd = true;
    } else if (result->inner_is_save_pfs && result->inner_mount[0]) {
        unmount_pfs(result->inner_mount);
    }

    snprintf(result->inner_mount, sizeof(result->inner_mount), "%s", try_mount);
    snprintf(result->inner_image_path, sizeof(result->inner_image_path), "%s", file_path);
    result->inner_is_save_pfs = true;
    result->inner_is_lvd_pfs = false;
    result->lvd_inner_unit = -1;

    if (had_lvd)
        unmount_lvd_pfs(old_mount, old_unit);
}

bool remount_pfsc_inner_pfs_via_save_data(pfsc_mount_result_t *result, const char *stage_dir) {
    char try_mount[MAX_PATH];
    char local_pfs[MAX_PATH];
    bool is_ffpfsc = result && result->outer_image_path[0] &&
                     strstr(result->outer_image_path, ".ffpfsc");

    if (!result || !result->inner_image_path[0])
        return false;
    if (result->inner_is_save_pfs)
        return true;

    if (is_ffpfsc) {
        mkdir("/data/imgmnt/pfscache", 0755);
        snprintf(local_pfs, sizeof(local_pfs), "/data/imgmnt/pfscache/%08x_pfs_image.dat",
                 fnv1a32(result->outer_image_path));
        if (access(local_pfs, F_OK) != 0) {
            if (link(result->inner_image_path, local_pfs) == 0) {
                di_logf("hardlinked inner PFS for save-data probe: %s", local_pfs);
            } else {
                di_logf("hardlink unavailable for save-data (%s -> %s): %s",
                        result->inner_image_path, local_pfs, strerror(errno));
            }
        }
        if (access(local_pfs, F_OK) == 0) {
            di_logf("trying save-data on pfscache link: %s", local_pfs);
            if (try_save_data_candidate(result, local_pfs, try_mount, sizeof(try_mount))) {
                adopt_save_data_mount(result, local_pfs, try_mount);
                di_logf("PFSC inner PFS switched to save-data at %s", result->inner_mount);
                return true;
            }
        }
    }

    di_logf("probing save-data inner without dropping LVD: %s", result->inner_image_path);
    if (try_save_data_candidate(result, result->inner_image_path, try_mount, sizeof(try_mount))) {
        adopt_save_data_mount(result, result->inner_image_path, try_mount);
        di_logf("PFSC inner PFS switched to save-data at %s", result->inner_mount);
        return true;
    }

    if (result->outer_image_path[0] && !is_ffpfsc) {
        di_logf("trying save-data on outer PFSC file: %s", result->outer_image_path);
        if (try_save_data_candidate(result, result->outer_image_path, try_mount, sizeof(try_mount))) {
            adopt_save_data_mount(result, result->outer_image_path, try_mount);
            di_logf("PFSC inner PFS switched to save-data outer at %s", result->inner_mount);
            return true;
        }
    }

    if (stage_dir && stage_dir[0]) {
        snprintf(local_pfs, sizeof(local_pfs), "%s/pfs_image.dat", stage_dir);
        if (access(local_pfs, F_OK) == 0) {
            di_logf("trying save-data on staged copy: %s", local_pfs);
            if (try_save_data_candidate(result, local_pfs, try_mount, sizeof(try_mount))) {
                adopt_save_data_mount(result, local_pfs, try_mount);
                di_logf("PFSC inner PFS switched to staged save-data at %s", result->inner_mount);
                return true;
            }
        }
    }

    if (result->inner_is_lvd_pfs && result->inner_mount[0]) {
        di_logf("save-data unavailable, keeping LVD inner PFS at %s", result->inner_mount);
    }

    return false;
}

bool remount_pfsc_inner_pfs_from_outer_offset(pfsc_mount_result_t *result) {
    struct stat inner_st;
    uint64_t region_off = 0;
    char old_mount[MAX_PATH];
    const char *outer_ext;

    if (!result || !result->outer_image_path[0] || !result->inner_image_path[0] ||
        !result->inner_is_lvd_pfs || !result->inner_mount[0])
        return false;

    if (stat(result->inner_image_path, &inner_st) != 0 || inner_st.st_size <= 0)
        return false;

    outer_ext = strrchr(result->outer_image_path, '.');
    if (outer_ext && !strcasecmp(outer_ext, ".ffpfsc")) {
        di_logf("offset remount skipped for compressed PFSC: %s", result->outer_image_path);
        return false;
    }

    if (!find_subfile_offset_in_container(result->outer_image_path, result->inner_image_path,
                                          (uint64_t)inner_st.st_size, &region_off))
        return false;

    snprintf(old_mount, sizeof(old_mount), "%s", result->inner_mount);
    unmount_lvd_pfs(old_mount, result->lvd_inner_unit);
    result->lvd_inner_unit = -1;
    result->inner_mount[0] = '\0';

    if (!mount_lvd_pfs_region(result->outer_image_path, region_off,
                              (uint64_t)inner_st.st_size, "/data/imgmnt/pfsmnt",
                              result->inner_mount, &result->lvd_inner_unit) ||
        !path_has_sce_sys(result->inner_mount)) {
        if (result->inner_mount[0])
            unmount_lvd_pfs(result->inner_mount, result->lvd_inner_unit);
        result->inner_mount[0] = '\0';
        result->lvd_inner_unit = -1;
        return false;
    }

    di_logf("PFSC inner PFS remounted via outer offset %llu at %s",
            (unsigned long long)region_off, result->inner_mount);
    return true;
}

void unmount_pfsc_result(pfsc_mount_result_t *result) {
    if (!result)
        return;

    if (result->inner_mount[0]) {
        if (result->inner_is_exfat)
            unmount_exfat(result->inner_mount);
        else if (result->inner_is_ufs)
            unmount_ufs(result->inner_mount);
        else if (result->inner_is_save_pfs)
            unmount_pfs(result->inner_mount);
        else if (result->inner_is_lvd_pfs)
            unmount_lvd_pfs(result->inner_mount, result->lvd_inner_unit);
        result->inner_mount[0] = '\0';
        result->lvd_inner_unit = -1;
    }

    if (result->outer_mount[0]) {
        unmount_lvd_pfs(result->outer_mount, result->lvd_outer_unit);
        result->outer_mount[0] = '\0';
        result->lvd_outer_unit = -1;
    }
}

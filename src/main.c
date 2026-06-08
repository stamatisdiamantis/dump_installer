#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/mount.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>

#include "types.h"
#include "image.h"
#include "hash.h"
#include "utils.h"
#include "mount_helpers.h"
#include "pfs_mount.h"
#include "ufs_mount.h"
#include "exfat_mount.h"
#include "install.h"
#include "lvd_mount.h"
#include "pfsc_resolve.h"
#include "log.h"

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallTitleDir(const char* title_id, const char* install_path, void* reserved);
int sceAppInstUtilAppInstallAll(void* reserved);
int sceAppInstUtilAppUnInstall(const char* title_id);

typedef struct {
    bool has_image;
    bool is_ufs;
    bool is_pfs;
    bool is_exfat;
    bool is_pfsc;
    char mount_point[MAX_PATH];
    pfsc_mount_result_t pfsc;
} mount_state_t;

typedef struct {
    char title_id[32];
    char src_sce_sys[MAX_PATH];
    bool has_snd0;
} staged_install_t;

static void cleanup_system_ex_mount(const char *title_id);

static void mount_state_init(mount_state_t *state) {
    memset(state, 0, sizeof(*state));
    pfsc_mount_result_init(&state->pfsc);
}

static void cleanup_mount_state(mount_state_t *state) {
    if (!state)
        return;

    if (state->is_pfsc) {
        unmount_pfsc_result(&state->pfsc);
        state->is_pfsc = false;
        return;
    }

    if (state->mount_point[0]) {
        if (state->is_ufs)
            unmount_ufs(state->mount_point);
        else if (state->is_pfs)
            unmount_pfs(state->mount_point);
        else if (state->is_exfat)
            unmount_exfat(state->mount_point);
        state->mount_point[0] = '\0';
    }
}

static bool title_id_from_image_path(const char *image_file, char *title_id, size_t size) {
    const char *base = strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file;

    if ((strncmp(base, "PPSA", 4) != 0 && strncmp(base, "CUSA", 4) != 0) || strlen(base) < 9)
        return false;

    snprintf(title_id, size, "%.9s", base);
    return true;
}

static bool read_link_file_target(const char *lnk_path, char *target, size_t target_sz) {
    FILE *f = fopen(lnk_path, "r");
    size_t len;

    if (!f)
        return false;

    if (!fgets(target, target_sz, f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    len = strlen(target);
    while (len > 0 && (target[len - 1] == '\n' || target[len - 1] == '\r')) {
        target[len - 1] = '\0';
        len--;
    }

    return target[0] != '\0';
}

static bool game_is_live_at_system_ex(const char *title_id) {
    char eboot[MAX_PATH];

    snprintf(eboot, sizeof(eboot), "/system_ex/app/%s/eboot.bin", title_id);
    return access(eboot, F_OK) == 0;
}

static bool system_ex_title_is_mounted(const char *title_id) {
    char path[MAX_PATH];
    struct statfs sfs;

    snprintf(path, sizeof(path), "/system_ex/app/%s", title_id);
    return statfs(path, &sfs) == 0 && strcmp(sfs.f_mntonname, path) == 0;
}

static bool user_app_has_registration_assets(const char *title_id) {
    char path[MAX_PATH];

    snprintf(path, sizeof(path), "/user/app/%s/sce_sys/param.sfo", title_id);
    if (access(path, F_OK) != 0)
        return false;

    snprintf(path, sizeof(path), "/user/appmeta/%s/icon0.png", title_id);
    return access(path, F_OK) == 0;
}

static bool game_has_valid_mount_link(const char *title_id, char *target, size_t target_sz) {
    char path[MAX_PATH];

    snprintf(path, sizeof(path), "/user/app/%s/mount.lnk", title_id);
    return access(path, F_OK) == 0 &&
           read_link_file_target(path, target, target_sz) &&
           access(target, F_OK) == 0;
}

static bool game_is_already_installed(const char *title_id) {
    char target[MAX_PATH];

    if (!game_is_live_at_system_ex(title_id))
        return false;

    if (!system_ex_title_is_mounted(title_id)) {
        di_logf("reinstall %s: eboot visible but /system_ex mount inactive", title_id);
        return false;
    }

    if (!game_has_valid_mount_link(title_id, target, sizeof(target))) {
        di_logf("reinstall %s: missing or invalid mount.lnk", title_id);
        return false;
    }

    if (!user_app_has_registration_assets(title_id)) {
        di_logf("reinstall %s: missing user/app or appmeta registration assets", title_id);
        return false;
    }

    di_logf("skip %s: live mount + eboot + mount.lnk + metadata", title_id);
    return true;
}

static void purge_title_install_files(const char *title_id) {
    char path[MAX_PATH];
    struct stat st;

    cleanup_system_ex_mount(title_id);

    snprintf(path, sizeof(path), "/user/app/%s", title_id);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        di_logf("purge: clearing %s", path);
        remove_dir(path);
    }

    snprintf(path, sizeof(path), "/user/appmeta/%s", title_id);
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        di_logf("purge: clearing %s", path);
        remove_dir(path);
    }
}

static void prepare_title_reinstall(const char *title_id) {
    int ret;

    if (game_is_already_installed(title_id))
        return;

    ret = sceAppInstUtilAppUnInstall(title_id);
    if (ret == 0)
        di_logf("AppUnInstall removed library entry for %s", title_id);
    else
        di_logf("AppUnInstall for %s: 0x%08x", title_id, ret);

    purge_title_install_files(title_id);
}

static int purge_grey_titles_in_dir(const char *dir) {
    dir_image_t images[MAX_DIR_IMAGES];
    int image_count = list_images_in_dir(dir, images, MAX_DIR_IMAGES);
    int purged = 0;

    if (image_count <= 0) {
        notify("No game files found to purge");
        di_logf("purge-grey: no installable items in %s", dir);
        return -1;
    }

    sceAppInstUtilInitialize();

    for (int i = 0; i < image_count; i++) {
        char title_id[32] = {};

        if (!title_id_from_image_path(images[i].path, title_id, sizeof(title_id))) {
            di_logf("purge-grey: skip %s (no title id in name)", images[i].path);
            continue;
        }

        notify("Removing %s from library...", title_id);
        prepare_title_reinstall(title_id);
        purged++;
    }

    notify("Removed %d grey/broken entries", purged);
    di_logf("purge-grey: finished %d title(s) in %s", purged, dir);
    return purged > 0 ? 0 : -1;
}

static int batch_install_priority(const dir_image_t *img) {
    if (!img)
        return 1;
    if (img->type == IMAGE_TYPE_EXFAT)
        return 2;
    if (img->type == IMAGE_TYPE_PFSC && strstr(img->path, ".exfat."))
        return 2;
    return 0;
}

static void sort_images_for_batch(dir_image_t *images, int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (batch_install_priority(&images[i]) > batch_install_priority(&images[j])) {
                dir_image_t tmp = images[i];
                images[i] = images[j];
                images[j] = tmp;
            }
        }
    }
}

static void build_stage_dir(const char *cwd, const char *image_file,
                            char *stage_dir, size_t stage_dir_sz) {
    snprintf(stage_dir, stage_dir_sz, "%s/.di_stage_%08x", cwd, fnv1a32(image_file));
}

static void cleanup_stage_dir(const char *stage_dir) {
    if (stage_dir && stage_dir[0])
        remove_dir(stage_dir);
}

static bool stage_sce_sys_from_mount(const char *mount_point, const char *stage_dir,
                                     char *src_sce_sys_out, size_t src_sce_sys_sz) {
    char src_sce_sys_tmp[MAX_PATH];
    snprintf(src_sce_sys_tmp, sizeof(src_sce_sys_tmp), "%s/sce_sys", mount_point);
    cleanup_stage_dir(stage_dir);
    mkdir(stage_dir, 0755);
    snprintf(src_sce_sys_out, src_sce_sys_sz, "%s/sce_sys", stage_dir);
    mkdir(src_sce_sys_out, 0755);
    return copy_dir(src_sce_sys_tmp, src_sce_sys_out) == 0;
}

static void write_link_file(const char *path, const char *value) {
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%s", value);
    fclose(f);
}

static void stage_user_app_assets(const char *title_id, const char *src_sce_sys) {
    char user_app_dir[MAX_PATH];
    char user_sce_sys[MAX_PATH];
    char icon_src[MAX_PATH];
    char icon_dst[MAX_PATH];

    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    mkdir(user_app_dir, 0755);

    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_sce_sys, 0755);
    copy_dir(src_sce_sys, user_sce_sys);

    snprintf(icon_src, sizeof(icon_src), "%s/icon0.png", src_sce_sys);
    snprintf(icon_dst, sizeof(icon_dst), "%s/icon0.png", user_app_dir);
    copy_file(icon_src, icon_dst);
}

static void cleanup_system_ex_mount(const char *title_id) {
    char system_ex_app[MAX_PATH];
    struct statfs sfs;

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    for (int i = 0; i < 8; i++) {
        if (statfs(system_ex_app, &sfs) != 0 || strcmp(sfs.f_mntonname, system_ex_app) != 0)
            break;
        di_logf("cleanup unmount %s (%s)", system_ex_app, sfs.f_fstypename);
        if (unmount(system_ex_app, MNT_FORCE) != 0 && errno != ENOENT && errno != EINVAL)
            break;
    }
}

static bool verify_system_ex_eboot(const char *title_id, const char *system_ex_app) {
    char eboot_dst[MAX_PATH];
    snprintf(eboot_dst, sizeof(eboot_dst), "%s/eboot.bin", system_ex_app);
    if (access(eboot_dst, F_OK) == 0)
        return true;

    di_logf("eboot.bin not visible at %s", eboot_dst);
    cleanup_system_ex_mount(title_id);
    notify("Mount verify failed for %s", title_id);
    return false;
}

static const char *resolve_mount_lnk_target(const char *system_ex_app, const char *nullfs_src,
                                            bool has_image, const mount_state_t *mounts) {
    if (!has_image)
        return nullfs_src;

    if (mounts->is_pfsc &&
        (mounts->pfsc.inner_is_save_pfs || mounts->pfsc.inner_is_lvd_pfs))
        return mounts->pfsc.inner_mount;

    if (mounts->is_pfsc && (mounts->pfsc.inner_is_exfat || mounts->pfsc.inner_is_ufs))
        return system_ex_app;

    if (mounts->is_exfat || mounts->is_ufs || mounts->is_pfs)
        return system_ex_app;

    return nullfs_src;
}

static bool overlay_pfs_staging_to_system_ex(const char *title_id, const char *system_ex_app,
                                             const char *staging) {
    char bridge[MAX_PATH];

    if (!staging || !staging[0]) {
        di_logf("nullfs staging path missing for %s", title_id);
        return false;
    }

    if (prepare_system_ex_for_nullfs(title_id) != 0) {
        notify("Failed to prepare %s", system_ex_app);
        return false;
    }

    di_logf("nullfs overlay: %s -> %s", staging, system_ex_app);
    if (mount_overlay_to_system_ex(staging, system_ex_app))
        return true;

    snprintf(bridge, sizeof(bridge), "/data/imgmnt/bridgemnt/%s", title_id);
    mkdir("/data/imgmnt/bridgemnt", 0755);
    di_logf("trying bridge nullfs via %s", bridge);
    return mount_overlay_via_bridge(staging, system_ex_app, bridge);
}

static bool mount_pfsc_pfs_to_system_ex(const char *title_id, const char *system_ex_app,
                                        const char *stage_dir, mount_state_t *mounts) {
    const char *staging = mounts->pfsc.inner_mount;
    char eboot_staging[MAX_PATH];

    if (!mounts->pfsc.inner_mount[0]) {
        di_logf("inner PFS staging missing for %s", title_id);
        return false;
    }

    if (!mounts->pfsc.inner_is_save_pfs)
        remount_pfsc_inner_pfs_via_save_data(&mounts->pfsc, stage_dir);

    staging = mounts->pfsc.inner_mount;
    snprintf(eboot_staging, sizeof(eboot_staging), "%s/eboot.bin", staging);
    if (access(eboot_staging, F_OK) != 0) {
        di_logf("eboot.bin missing at %s", eboot_staging);
        notify("Missing eboot.bin for %s", title_id);
        return false;
    }

    if (mounts->pfsc.inner_is_save_pfs) {
        di_logf("using save-data inner PFS at %s for nullfs", staging);
        if (mount_title_nullfs(title_id, staging))
            return verify_system_ex_eboot(title_id, system_ex_app);
    } else if (mounts->pfsc.inner_is_lvd_pfs) {
        di_logf("using LVD inner PFS at %s for nullfs overlay", staging);
        if (overlay_pfs_staging_to_system_ex(title_id, system_ex_app, staging))
            return verify_system_ex_eboot(title_id, system_ex_app);
    }

    notify("Mount failed for %s", title_id);
    cleanup_system_ex_mount(title_id);
    return false;
}

static bool mount_game_to_system_ex(const char *title_id, char *system_ex_app,
                                    const char *stage_dir, const char *nullfs_src,
                                    const char *image_file, mount_state_t *mounts,
                                    bool has_image) {
    if (has_image && mounts->is_pfsc &&
        (mounts->pfsc.inner_is_save_pfs || mounts->pfsc.inner_is_lvd_pfs)) {
        if (!mount_pfsc_pfs_to_system_ex(title_id, system_ex_app, stage_dir, mounts))
            return false;
        return true;
    }

    if (!has_image) {
        if (!mount_title_nullfs(title_id, nullfs_src)) {
            notify("nullfs mount failed for %s", title_id);
            return false;
        }
        return true;
    }

    if (prepare_system_ex_mount_point(title_id) != 0) {
        di_logf("prepare_system_ex_mount_point failed for %s: errno=%d (%s)",
                system_ex_app, errno, strerror(errno));
        notify("Failed to prepare %s: %s", system_ex_app, strerror(errno));
        return false;
    }

    if (mounts->is_pfsc && mounts->pfsc.inner_is_exfat) {
        unmount_exfat(mounts->pfsc.inner_mount);
        mounts->pfsc.inner_mount[0] = '\0';
        if (!mount_exfat_image_at(mounts->pfsc.inner_image_path, system_ex_app, true)) {
            notify("exFAT mount failed for %s", title_id);
            return false;
        }
        snprintf(mounts->pfsc.inner_mount, sizeof(mounts->pfsc.inner_mount), "%s", system_ex_app);
        mounts->pfsc.inner_is_exfat = true;
        return true;
    }

    if (mounts->is_pfsc && mounts->pfsc.inner_is_ufs) {
        unmount_ufs(mounts->pfsc.inner_mount);
        mounts->pfsc.inner_mount[0] = '\0';
        if (!mount_ufs_image_ex(mounts->pfsc.inner_image_path, system_ex_app, true)) {
            notify("UFS mount failed for %s", title_id);
            return false;
        }
        snprintf(mounts->pfsc.inner_mount, sizeof(mounts->pfsc.inner_mount), "%s", system_ex_app);
        mounts->pfsc.inner_is_ufs = true;
        return true;
    }

    if (mounts->is_exfat) {
        unmount_exfat(mounts->mount_point);
        mounts->mount_point[0] = '\0';
        if (!mount_exfat_image_ex(image_file, system_ex_app, true)) {
            notify("exFAT mount failed for %s", title_id);
            return false;
        }
        return true;
    }

    if (mounts->is_ufs) {
        unmount_ufs(mounts->mount_point);
        mounts->mount_point[0] = '\0';
        if (!mount_ufs_image_ex(image_file, system_ex_app, true)) {
            notify("UFS mount failed for %s", title_id);
            return false;
        }
        return true;
    }

    if (mounts->is_pfs) {
        unmount_pfs(mounts->mount_point);
        mounts->mount_point[0] = '\0';
        if (!mount_pfs_image(image_file, system_ex_app)) {
            notify("PFS mount failed for %s", title_id);
            return false;
        }
        return true;
    }

    if (!mount_title_nullfs(title_id, nullfs_src)) {
        notify("nullfs mount failed for %s", title_id);
        return false;
    }

    return true;
}

static void mount_state_from_type(mount_state_t *mounts, image_type_t type) {
    mounts->has_image = type != IMAGE_TYPE_FOLDER;
    mounts->is_ufs = type == IMAGE_TYPE_UFS;
    mounts->is_pfs = type == IMAGE_TYPE_PFS;
    mounts->is_exfat = type == IMAGE_TYPE_EXFAT;
    mounts->is_pfsc = type == IMAGE_TYPE_PFSC;
}

static bool prepare_image_mount(const char *stage_dir, const char *image_file, image_type_t type,
                                mount_state_t *mounts, const char **nullfs_src_out,
                                char *src_sce_sys, size_t src_sce_sys_sz) {
    const char *image_name = strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file;
    const char *nullfs_src = NULL;

    mount_state_init(mounts);
    mount_state_from_type(mounts, type);

    if (type == IMAGE_TYPE_PFSC) {
        if (mount_pfsc_container(image_file, &mounts->pfsc) &&
            resolve_nested_game_image(&mounts->pfsc)) {
            nullfs_src = mounts->pfsc.inner_mount;
            snprintf(mounts->mount_point, sizeof(mounts->mount_point), "%s",
                     mounts->pfsc.inner_mount);
            if (!stage_sce_sys_from_mount(nullfs_src, stage_dir, src_sce_sys, src_sce_sys_sz)) {
                notify("Warning: sce_sys copy from PFSC nested mount failed");
                cleanup_mount_state(mounts);
                mounts->is_pfsc = false;
                return false;
            }
        } else {
            notify("PFSC mount failed - skipping %s", image_name);
            cleanup_mount_state(mounts);
            return false;
        }
    } else if (type == IMAGE_TYPE_UFS) {
        if (mount_ufs_image(image_file, mounts->mount_point)) {
            nullfs_src = mounts->mount_point;
            if (!stage_sce_sys_from_mount(nullfs_src, stage_dir, src_sce_sys, src_sce_sys_sz)) {
                notify("Warning: sce_sys copy from UFS failed");
                cleanup_mount_state(mounts);
                return false;
            }
        } else {
            notify("UFS mount failed - skipping %s", image_name);
            return false;
        }
    } else if (type == IMAGE_TYPE_PFS) {
        if (mount_pfs_image(image_file, mounts->mount_point)) {
            nullfs_src = mounts->mount_point;
            if (!stage_sce_sys_from_mount(nullfs_src, stage_dir, src_sce_sys, src_sce_sys_sz)) {
                notify("Failed to copy sce_sys from PFS - skipping %s", image_name);
                cleanup_mount_state(mounts);
                return false;
            }
        } else {
            notify("PFS mount failed - skipping %s", image_name);
            return false;
        }
    } else if (type == IMAGE_TYPE_EXFAT) {
        if (mount_exfat_image(image_file, mounts->mount_point)) {
            nullfs_src = mounts->mount_point;
            if (!stage_sce_sys_from_mount(nullfs_src, stage_dir, src_sce_sys, src_sce_sys_sz)) {
                notify("Warning: sce_sys copy from ExFat failed");
                cleanup_mount_state(mounts);
                return false;
            }
        } else {
            notify("ExFat mount failed - skipping %s", image_name);
            return false;
        }
    }

    *nullfs_src_out = nullfs_src;
    return true;
}

static bool stage_has_snd0(const char *src_sce_sys) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/snd0.at9", src_sce_sys);
    return access(path, F_OK) == 0;
}

static int register_staged_titles(staged_install_t *staged, int staged_count, bool batch_mode) {
    int install_ret;
    int i;

    if (staged_count <= 0)
        return 0;

    if (batch_mode) {
        di_logf("batch register: AppInstallAll for %d title(s)", staged_count);
        notify("Registering %d games...", staged_count);
        install_ret = sceAppInstUtilAppInstallAll(0);
        if (install_ret == 0) {
            di_logf("AppInstallAll ok for %d title(s)", staged_count);
            return 0;
        }
        di_logf("AppInstallAll failed: 0x%08x, trying TitleDir per title", install_ret);
    }

    for (i = 0; i < staged_count; i++) {
        install_ret = sceAppInstUtilAppInstallTitleDir(staged[i].title_id, "/user/app/", 0);
        if (install_ret != 0) {
            di_logf("TitleDir failed for %s: 0x%08x", staged[i].title_id, install_ret);
            return install_ret;
        }
        di_logf("TitleDir succeeded for %s", staged[i].title_id);
    }

    return 0;
}

static void finalize_staged_installs(staged_install_t *staged, int staged_count) {
    int i;

    for (i = 0; i < staged_count; i++) {
        char user_sce_sys[MAX_PATH];

        snprintf(user_sce_sys, sizeof(user_sce_sys), "/user/app/%s/sce_sys",
                 staged[i].title_id);
        copy_sce_sys_to_appmeta(user_sce_sys, staged[i].title_id);
        update_trophy(staged[i].title_id, user_sce_sys);

        if (staged[i].has_snd0) {
            int snd0_updates = update_snd0info(staged[i].title_id);
            if (snd0_updates >= 0)
                di_logf("snd0info updated for %s rows=%d", staged[i].title_id, snd0_updates);
        }
    }
}

static int install_one(const char *cwd, const dir_image_t *image, bool defer_register,
                       staged_install_t *staged_out) {
    char title_id[32] = {};
    char system_ex_app[MAX_PATH];
    char user_app_dir[MAX_PATH];
    char src_sce_sys[MAX_PATH];
    char stage_dir[MAX_PATH] = {};
    char mount_lnk_path[MAX_PATH];
    char mount_img_lnk_path[MAX_PATH];
    char sce_sys_path[MAX_PATH];

    const bool has_image = image && image->type != IMAGE_TYPE_FOLDER;
    const char *image_file = has_image ? image->path : NULL;
    image_type_t type = has_image ? image->type : IMAGE_TYPE_FOLDER;

    mount_state_t mounts;
    const char *nullfs_src = cwd;

    di_logf("--- install start ---");
    di_logf("cwd=%s", cwd);

    if (has_image && image_file &&
        title_id_from_image_path(image_file, title_id, sizeof(title_id)) &&
        game_is_already_installed(title_id)) {
        di_logf("skipping %s: already installed", title_id);
        notify("Skipping %s (already installed)", title_id);
        return 1;
    }

    if (has_image) {
        di_logf("image=%s type=%d", image_file, (int)type);
        build_stage_dir(cwd, image_file, stage_dir, sizeof(stage_dir));
        di_logf("stage_dir=%s", stage_dir);
        if (!prepare_image_mount(stage_dir, image_file, type, &mounts, &nullfs_src,
                                 src_sce_sys, sizeof(src_sce_sys))) {
            di_logf("prepare_image_mount failed");
            cleanup_stage_dir(stage_dir);
            return -1;
        }
    } else {
        di_logf("folder install (no image file)");
        snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
    }
    di_logf("nullfs_src=%s", nullfs_src);

    snprintf(sce_sys_path, sizeof(sce_sys_path), "%s/sce_sys", nullfs_src);
    if (access(sce_sys_path, F_OK) != 0) {
        if (has_image) {
            const char *image_name = strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file;
            notify("No sce_sys for %s - skipping", image_name);
        } else {
            notify("CRITICAL: No sce_sys folder at %s - cannot read Title ID", sce_sys_path);
        }
        cleanup_mount_state(&mounts);
        cleanup_stage_dir(stage_dir);
        return -1;
    }

    if (has_image && stage_dir[0] &&
        get_title_id(stage_dir, title_id, sizeof(title_id)) == 0) {
        di_logf("title_id from staged sce_sys: %s", title_id);
    } else if (get_title_id(nullfs_src, title_id, sizeof(title_id)) == 0) {
        di_logf("title_id from mount: %s", title_id);
    } else if (has_image && image_file &&
               title_id_from_image_path(image_file, title_id, sizeof(title_id))) {
        di_logf("title_id from image filename: %s", title_id);
    } else {
        if (has_image) {
            const char *image_name = strrchr(image_file, '/') ?
                                    strrchr(image_file, '/') + 1 : image_file;
            notify("Failed to read Title ID from %s - skipping", image_name);
        } else {
            notify("Failed to read Title ID from %s/sce_sys", nullfs_src);
        }
        cleanup_mount_state(&mounts);
        cleanup_stage_dir(stage_dir);
        return -1;
    }

    di_logf("title_id=%s", title_id);

    if (game_is_already_installed(title_id)) {
        di_logf("skipping %s: already installed (verified from sce_sys)", title_id);
        notify("Skipping %s (already installed)", title_id);
        cleanup_mount_state(&mounts);
        cleanup_stage_dir(stage_dir);
        return 1;
    }

    notify("Installing %s, please wait...", title_id);

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    di_logf("system_ex_app=%s", system_ex_app);

    prepare_title_reinstall(title_id);

    if (access(nullfs_src, F_OK) != 0) {
        notify("Install source missing for %s: %s", title_id, nullfs_src);
        cleanup_mount_state(&mounts);
        cleanup_stage_dir(stage_dir);
        return -1;
    }

    if (!mount_game_to_system_ex(title_id, system_ex_app, stage_dir, nullfs_src, image_file,
                                 &mounts, has_image)) {
        di_logf("mount_game_to_system_ex failed for %s", title_id);
        cleanup_system_ex_mount(title_id);
        cleanup_mount_state(&mounts);
        cleanup_stage_dir(stage_dir);
        return -1;
    }
    di_logf("mounted game content at %s", system_ex_app);

    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    stage_user_app_assets(title_id, src_sce_sys);
    copy_sce_sys_to_appmeta(src_sce_sys, title_id);
    update_trophy(title_id, src_sce_sys);

    {
        const char *mount_lnk_target =
            resolve_mount_lnk_target(system_ex_app, nullfs_src, has_image, &mounts);

        snprintf(mount_lnk_path, sizeof(mount_lnk_path), "/user/app/%s/mount.lnk", title_id);
        write_link_file(mount_lnk_path, mount_lnk_target);
        di_logf("mount.lnk -> %s", mount_lnk_target);

        if (has_image) {
            snprintf(mount_img_lnk_path, sizeof(mount_img_lnk_path),
                     "/user/app/%s/mount_img.lnk", title_id);
            write_link_file(mount_img_lnk_path, image_file);
            di_logf("mount_img.lnk -> %s", image_file);
        }
    }

    cleanup_stage_dir(stage_dir);

    if (defer_register) {
        if (staged_out) {
            char user_sce_sys[MAX_PATH];

            snprintf(staged_out->title_id, sizeof(staged_out->title_id), "%s", title_id);
            snprintf(user_sce_sys, sizeof(user_sce_sys), "/user/app/%s/sce_sys", title_id);
            snprintf(staged_out->src_sce_sys, sizeof(staged_out->src_sce_sys), "%s", user_sce_sys);
            staged_out->has_snd0 = stage_has_snd0(user_sce_sys);
        }
        di_logf("staged %s for batch register", title_id);
        return 0;
    }

    int install_ret = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);

    if (install_ret != 0) {
        di_logf("TitleDir failed for %s: 0x%08x", title_id, install_ret);
        notify("TitleDir failed for %s: 0x%08x", title_id, install_ret);
        install_ret = sceAppInstUtilAppInstallAll(0);

        if (install_ret != 0) {
            notify("AppInstallAll failed for %s: 0x%08x", title_id, install_ret);
            cleanup_system_ex_mount(title_id);
            cleanup_mount_state(&mounts);
            return -1;
        }

        di_logf("AppInstallAll fallback ok for %s", title_id);
    } else {
        di_logf("TitleDir succeeded for %s", title_id);
    }

    remount_system_ex();
    sleep(3);

    {
        char user_sce_sys[MAX_PATH];

        snprintf(user_sce_sys, sizeof(user_sce_sys), "/user/app/%s/sce_sys", title_id);
        copy_sce_sys_to_appmeta(user_sce_sys, title_id);
        update_trophy(title_id, user_sce_sys);

        if (stage_has_snd0(user_sce_sys)) {
            int snd0_updates = update_snd0info(title_id);
            if (snd0_updates >= 0)
                di_logf("snd0info updated for %s rows=%d", title_id, snd0_updates);
        }
    }

    notify("%s installed and ready to use!", title_id);
    return 0;
}

static int ensure_mount_dirs(void) {
    const char *dirs[] = {
        "/data/imgmnt",
        "/data/imgmnt/exfatmnt",
        "/data/imgmnt/pfsmnt",
        "/data/imgmnt/ufsmnt",
        "/data/imgmnt/pfscmnt",
        "/data/imgmnt/bridgemnt",
        "/data/imgmnt/dipayload",
        "/data/imgmnt/pfscache"
    };

    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        if (mkdir(dirs[i], 0755) != 0) {
            if (errno != EEXIST) {
                char parent[PATH_MAX];
                strncpy(parent, dirs[i], sizeof(parent) - 1);
                parent[sizeof(parent)-1] = '\0';

                char *last_slash = strrchr(parent, '/');
                if (last_slash && last_slash != parent) {
                    *last_slash = '\0';
                    mkdir(parent, 0755);
                }

                if (mkdir(dirs[i], 0755) != 0 && errno != EEXIST) {
                    printf("Failed to create %s (errno %d: %s)\n",
                           dirs[i], errno, strerror(errno));
                    return 1;
                }
            }
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    char cwd[PATH_MAX];
    int ret = 0;

    if (argc >= 2 && !strcmp(argv[1], "--cleanup-dilocs")) {
        cleanup_legacy_dilocs();
        notify("Removed /data/dilocs only");
        return 0;
    }

    if (argc >= 2 && !strcmp(argv[1], "--purge-grey")) {
        notify("Purging grey library entries...");
        if (!getcwd(cwd, sizeof(cwd))) {
            notify("Unable to determine working directory");
            return -1;
        }
        ret = purge_grey_titles_in_dir(cwd);
        notify("Run install again after grey icons disappear");
        return ret;
    }

    if (!getcwd(cwd, sizeof(cwd))) {
        di_logf("getcwd failed: errno=%d (%s)", errno, strerror(errno));
        printf("Error: Unable to determine working directory\n");
        ret = -1;
        goto done;
    }

    di_logf("working directory: %s", cwd);

    if (!strcmp(cwd, "/") || !strcmp(cwd, "//")) {
        notify("Select a folder with games, not console root");
        di_logf("refusing install from console root (cwd=%s)", cwd);
        ret = -1;
        goto done;
    }

    if (ensure_mount_dirs() != 0) {
        di_logf("ensure_mount_dirs failed");
        ret = 1;
        goto done;
    }

    reset_installer_session_mounts();
    mkdir("/system_ex/app", 0777);
    di_logf("remount_system_ex done");

    dir_image_t images[MAX_DIR_IMAGES];
    int image_count = list_images_in_dir(cwd, images, MAX_DIR_IMAGES);
    di_logf("found %d installable item(s) in cwd", image_count);
    for (int i = 0; i < image_count; i++) {
        di_logf("  [%d] %s (type=%d)", i, images[i].path, (int)images[i].type);
    }

    if (image_count > 1) {
        sort_images_for_batch(images, image_count);
        di_logf("batch install order (pfs-inner first, exfat-inner last):");
        for (int i = 0; i < image_count; i++)
            di_logf("  [%d] %s", i, images[i].path);
    }

    sceAppInstUtilInitialize();

    if (image_count > 0) {
        bool batch_mode = image_count > 1;
        staged_install_t staged[MAX_DIR_IMAGES];
        int staged_count = 0;
        int failures = 0;
        int skipped = 0;
        int installed = 0;

        if (batch_mode)
            notify("Found %d images, staging each...", image_count);

        for (int i = 0; i < image_count; i++) {
            char early_id[32] = {};
            di_logf("batch item %d/%d", i + 1, image_count);
            if (title_id_from_image_path(images[i].path, early_id, sizeof(early_id)) &&
                game_is_already_installed(early_id)) {
                di_logf("batch skip %s: already installed", early_id);
                notify("Skipping %s (already installed)", early_id);
                skipped++;
                continue;
            }

            staged_install_t *slot = batch_mode && staged_count < MAX_DIR_IMAGES ?
                                     &staged[staged_count] : NULL;
            int install_result = install_one(cwd, &images[i], batch_mode, slot);
            if (install_result < 0)
                failures++;
            else if (install_result > 0)
                skipped++;
            else {
                installed++;
                if (batch_mode && slot)
                    staged_count++;
            }
        }

        if (batch_mode && staged_count > 0) {
            int reg_ret = register_staged_titles(staged, staged_count, batch_mode);
            if (reg_ret != 0) {
                notify("Batch register failed: 0x%08x", reg_ret);
                di_logf("batch register failed: 0x%08x", reg_ret);
                ret = -1;
                goto done;
            }
            sleep(3);
            finalize_staged_installs(staged, staged_count);
        }

        if (failures > 0) {
            di_logf("batch finished: %d installed, %d skipped, %d failed of %d",
                    installed, skipped, failures, image_count);
            notify("%d of %d installs failed", failures, image_count);
            ret = -1;
            goto done;
        }

        if (skipped > 0 && installed > 0)
            notify("Installed %d new, skipped %d already installed", installed, skipped);
        else if (skipped > 0 && installed == 0)
            notify("All %d games already installed", skipped);
        else if (image_count > 1)
            notify("All %d games installed!", image_count);

        ret = 0;
        goto done;
    }

    ret = install_one(cwd, NULL, false, NULL);

done:
    return ret;
}

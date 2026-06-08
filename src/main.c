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
#include <dirent.h>

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
#include "autoscan.h"

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

typedef enum {
    GAME_ACTION_SKIP,
    GAME_ACTION_REMOUNT,
    GAME_ACTION_INSTALL,
} game_action_t;

typedef struct {
    char title_id[32];
    char src_sce_sys[MAX_PATH];
    bool has_snd0;
    bool needs_register;
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

static bool title_id_for_image_path(const char *image_path, char *title_id, size_t size) {
    DIR *d;
    struct dirent *e;
    char lnk[MAX_PATH];
    char target[MAX_PATH];

    if (title_id_from_image_path(image_path, title_id, size))
        return true;

    d = opendir("/user/app");
    if (!d)
        return false;

    while ((e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        if (strncmp(e->d_name, "PPSA", 4) != 0 && strncmp(e->d_name, "CUSA", 4) != 0)
            continue;

        snprintf(lnk, sizeof(lnk), "/user/app/%s/mount_img.lnk", e->d_name);
        if (read_link_file_target(lnk, target, sizeof(target)) &&
            !strcmp(target, image_path)) {
            snprintf(title_id, size, "%s", e->d_name);
            closedir(d);
            return true;
        }
    }

    closedir(d);
    return false;
}

static bool game_is_live_at_system_ex(const char *title_id) {
    char eboot[MAX_PATH];

    snprintf(eboot, sizeof(eboot), "/system_ex/app/%s/eboot.bin", title_id);
    return access(eboot, F_OK) == 0;
}

static bool game_is_live(const char *title_id) {
    char lnk[MAX_PATH];
    char target[MAX_PATH];
    char eboot[MAX_PATH];

    if (game_is_live_at_system_ex(title_id))
        return true;

    snprintf(lnk, sizeof(lnk), "/user/app/%s/mount.lnk", title_id);
    if (!read_link_file_target(lnk, target, sizeof(target)))
        return false;

    snprintf(eboot, sizeof(eboot), "%s/eboot.bin", target);
    return access(eboot, F_OK) == 0;
}

static bool game_library_entry_exists(const char *title_id) {
    char path[MAX_PATH];

    snprintf(path, sizeof(path), "/user/app/%s/sce_sys/param.sfo", title_id);
    return access(path, F_OK) == 0;
}

static void normalize_image_path(const char *path, char *out, size_t out_sz) {
    size_t j = 0;

    if (!out || out_sz == 0)
        return;

    out[0] = '\0';
    if (!path || !path[0])
        return;

    if (path[0] == '/') {
        out[j++] = '/';
        path++;
    }

    while (*path && j + 1 < out_sz) {
        if (path[0] == '/' && path[1] == '/') {
            path++;
            continue;
        }
        out[j++] = *path++;
    }

    while (j > 1 && out[j - 1] == '/')
        j--;
    out[j] = '\0';
}

static bool image_paths_equal(const char *a, const char *b) {
    char na[MAX_PATH];
    char nb[MAX_PATH];

    normalize_image_path(a, na, sizeof(na));
    normalize_image_path(b, nb, sizeof(nb));
    return na[0] && nb[0] && !strcmp(na, nb);
}

static bool mount_img_matches_dump(const char *title_id, const char *image_path) {
    char path[MAX_PATH];
    char linked_image[MAX_PATH];

    if (!title_id || !title_id[0] || !image_path || !image_path[0])
        return false;

    snprintf(path, sizeof(path), "/user/app/%s/mount_img.lnk", title_id);
    return read_link_file_target(path, linked_image, sizeof(linked_image)) &&
           image_paths_equal(linked_image, image_path);
}

static void di_log_game_diagnostics(const char *label, const char *title_id,
                                    const char *image_path) {
    char path[MAX_PATH];
    char linked_image[MAX_PATH];
    char linked_mount[MAX_PATH];
    char eboot[MAX_PATH];
    char normalized_dump[MAX_PATH];
    char normalized_link[MAX_PATH];

    di_logf("--- diagnostics [%s] %s ---", label, title_id);
    di_logf("  dump_path: %s", image_path ? image_path : "(null)");

    snprintf(path, sizeof(path), "/user/app/%s/sce_sys/param.sfo", title_id);
    di_logf("  library(param.sfo): %s", access(path, F_OK) == 0 ? "yes" : "no");

    snprintf(path, sizeof(path), "/user/app/%s/mount_img.lnk", title_id);
    if (read_link_file_target(path, linked_image, sizeof(linked_image))) {
        normalize_image_path(image_path, normalized_dump, sizeof(normalized_dump));
        normalize_image_path(linked_image, normalized_link, sizeof(normalized_link));
        di_logf("  mount_img.lnk: %s", linked_image);
        di_logf("  mount_img match: %s (norm dump=%s norm lnk=%s)",
                image_paths_equal(linked_image, image_path) ? "yes" : "NO",
                normalized_dump, normalized_link);
    } else {
        di_logf("  mount_img.lnk: missing");
    }

    snprintf(path, sizeof(path), "/user/app/%s/mount.lnk", title_id);
    if (read_link_file_target(path, linked_mount, sizeof(linked_mount))) {
        snprintf(eboot, sizeof(eboot), "%s/eboot.bin", linked_mount);
        di_logf("  mount.lnk: %s (eboot=%s)", linked_mount,
                access(eboot, F_OK) == 0 ? "yes" : "no");
    } else {
        di_logf("  mount.lnk: missing");
    }

    snprintf(eboot, sizeof(eboot), "/system_ex/app/%s/eboot.bin", title_id);
    di_logf("  system_ex eboot: %s", access(eboot, F_OK) == 0 ? "yes" : "no");
    di_logf("  game_is_live: %s", game_is_live(title_id) ? "yes" : "no");
}

static bool batch_is_partial(const dir_image_t *images, int count) {
    bool has_registered = false;
    bool has_unregistered = false;

    for (int i = 0; i < count; i++) {
        char title_id[32] = {};
        bool library;
        bool link;

        if (!title_id_for_image_path(images[i].path, title_id, sizeof(title_id)))
            continue;

        bool live;

        library = game_library_entry_exists(title_id);
        link = mount_img_matches_dump(title_id, images[i].path);
        live = game_is_live(title_id);

        di_logf("  partial check %s: library=%d link=%d live=%d",
                title_id, library, link, live);

        if (live)
            has_registered = true;
        else
            has_unregistered = true;
    }

    di_logf("batch_is_partial: registered=%d unregistered=%d -> %s",
            has_registered, has_unregistered,
            (has_registered && has_unregistered) ? "yes" : "no");
    return has_registered && has_unregistered;
}

static game_action_t plan_game_action(const char *title_id, const char *image_path,
                                      bool partial_batch) {
    bool eboot_live;
    bool library;
    bool dump_present;
    bool link_matches;

    if (!title_id || !title_id[0] || !image_path || !image_path[0])
        return GAME_ACTION_INSTALL;

    dump_present = access(image_path, F_OK) == 0;
    if (!dump_present)
        return GAME_ACTION_INSTALL;

    eboot_live = game_is_live(title_id);
    library = game_library_entry_exists(title_id);
    link_matches = mount_img_matches_dump(title_id, image_path);

    di_logf("plan %s: library=%d link=%d eboot=%d partial=%d dump=%d",
            title_id, library, link_matches, eboot_live, partial_batch, dump_present);

    if (link_matches && eboot_live && dump_present) {
        if (!library) {
            di_logf("plan %s: -> REGISTER (live mount, missing library)", title_id);
            return GAME_ACTION_REMOUNT;
        }
        di_logf("plan %s: -> SKIP (live mount, linked dump)", title_id);
        return GAME_ACTION_SKIP;
    }

    if (eboot_live && dump_present && !link_matches) {
        if (!library) {
            di_logf("plan %s: -> INSTALL (orphan mount after delete)", title_id);
            return GAME_ACTION_INSTALL;
        }
        di_logf("plan %s: -> REPAIR (live mount, refresh links)", title_id);
        return GAME_ACTION_REMOUNT;
    }

    if (partial_batch && !eboot_live) {
        di_logf("plan %s: -> INSTALL (partial batch, not live)", title_id);
        return GAME_ACTION_INSTALL;
    }

    if (library && link_matches && dump_present) {
        di_logf("plan %s: -> REMOUNT (library entry, mount dead)", title_id);
        return GAME_ACTION_REMOUNT;
    }

    di_logf("plan %s: install (library=%d link=%d eboot=%d)",
            title_id, library, link_matches, eboot_live);
    return GAME_ACTION_INSTALL;
}

static void purge_title_install_files(const char *title_id) {
    char path[MAX_PATH];
    struct stat st;

    prepare_system_ex_mount_point(title_id);

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

    if (game_library_entry_exists(title_id)) {
        ret = sceAppInstUtilAppUnInstall(title_id);
        if (ret == 0)
            di_logf("AppUnInstall removed library entry for %s", title_id);
        else
            di_logf("AppUnInstall for %s: 0x%08x", title_id, ret);
    } else {
        di_logf("no library entry for %s, skipping AppUnInstall", title_id);
    }

    purge_title_install_files(title_id);
}

static void prepare_game_action(const char *title_id, game_action_t action) {
    if (!title_id || !title_id[0])
        return;

    if (action == GAME_ACTION_REMOUNT) {
        prepare_system_ex_title_reinstall(title_id);
        return;
    }

    if (action == GAME_ACTION_INSTALL)
        prepare_title_reinstall(title_id);
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

static int batch_install_urgency(const dir_image_t *img, game_action_t action) {
    char title_id[32] = {};

    if (action != GAME_ACTION_INSTALL || !img)
        return 0;
    if (!title_id_for_image_path(img->path, title_id, sizeof(title_id)))
        return 2;
    if (!game_is_live(title_id))
        return 0;
    if (!game_library_entry_exists(title_id) && !mount_img_matches_dump(title_id, img->path))
        return 1;
    return 2;
}

static int batch_action_priority(game_action_t action) {
    if (action == GAME_ACTION_INSTALL)
        return 0;
    if (action == GAME_ACTION_REMOUNT)
        return 1;
    return 2;
}

static void sort_batch_by_action(dir_image_t *images, game_action_t *actions, int count) {
    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            int ai = batch_action_priority(actions[i]);
            int aj = batch_action_priority(actions[j]);
            int ui = batch_install_urgency(&images[i], actions[i]);
            int uj = batch_install_urgency(&images[j], actions[j]);
            int bi = batch_install_priority(&images[i]);
            int bj = batch_install_priority(&images[j]);
            bool swap = false;

            if (ai > aj)
                swap = true;
            else if (ai == aj && ui > uj)
                swap = true;
            else if (ai == aj && ui == uj && bi > bj)
                swap = true;

            if (swap) {
                dir_image_t tmp_img = images[i];
                game_action_t tmp_act = actions[i];

                images[i] = images[j];
                actions[i] = actions[j];
                images[j] = tmp_img;
                actions[j] = tmp_act;
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
    char eboot[MAX_PATH];

    snprintf(eboot, sizeof(eboot), "%s/eboot.bin", system_ex_app);
    if (access(eboot, F_OK) == 0)
        return system_ex_app;

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

    snprintf(bridge, sizeof(bridge), "/data/imgmnt/bridgemnt/%s", title_id);
    mkdir("/data/imgmnt/bridgemnt", 0755);
    di_logf("bridge nullfs: %s -> %s via %s", staging, system_ex_app, bridge);
    if (mount_overlay_via_bridge(staging, system_ex_app, bridge))
        return true;

    di_logf("direct nullfs overlay: %s -> %s", staging, system_ex_app);
    return mount_overlay_to_system_ex(staging, system_ex_app);
}

#define SYSTEM_EX_NULLFS_LIMIT 3

static int count_system_ex_nullfs_mounts(void) {
    struct statfs *mntbuf = NULL;
    int count = 0;
    int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);

    if (mntcount <= 0 || !mntbuf)
        return 0;

    for (int i = 0; i < mntcount; i++) {
        if (strncmp(mntbuf[i].f_mntonname, "/system_ex/app/", 15) == 0 &&
            strcmp(mntbuf[i].f_fstypename, "nullfs") == 0)
            count++;
    }

    return count;
}

static bool system_ex_nullfs_mount_active(const char *title_id) {
    char path[MAX_PATH];
    struct statfs sfs;

    if (!title_id || !title_id[0])
        return false;

    snprintf(path, sizeof(path), "/system_ex/app/%s", title_id);
    if (statfs(path, &sfs) != 0 || strcmp(sfs.f_mntonname, path) != 0)
        return false;

    return strcmp(sfs.f_fstypename, "nullfs") == 0;
}

static bool system_ex_has_stale_nullfs(const char *title_id) {
    char eboot[MAX_PATH];

    if (!system_ex_nullfs_mount_active(title_id))
        return false;

    snprintf(eboot, sizeof(eboot), "/system_ex/app/%s/eboot.bin", title_id);
    return access(eboot, F_OK) != 0;
}

static void reclaim_stale_nullfs_for_title(const char *title_id) {
    if (!system_ex_has_stale_nullfs(title_id))
        return;

    di_logf("reclaiming stale nullfs slot for %s (mount present, no eboot)", title_id);
    cleanup_system_ex_mount(title_id);
    purge_title_install_files(title_id);
}

static void cleanup_stale_system_ex_nullfs_mounts(void) {
    struct statfs *mntbuf = NULL;
    int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);
    char seen[MAX_DIR_IMAGES][32];
    int seen_count = 0;

    if (mntcount <= 0 || !mntbuf)
        return;

    for (int i = 0; i < mntcount; i++) {
        const char *on = mntbuf[i].f_mntonname;
        char title_id[32];
        char *slash;
        bool already;

        if (strncmp(on, "/system_ex/app/", 15) != 0)
            continue;
        if (strcmp(mntbuf[i].f_fstypename, "nullfs") != 0)
            continue;

        snprintf(title_id, sizeof(title_id), "%s", on + 15);
        slash = strchr(title_id, '/');
        if (slash)
            *slash = '\0';
        if (!title_id[0])
            continue;

        already = false;
        for (int j = 0; j < seen_count; j++) {
            if (!strcmp(seen[j], title_id)) {
                already = true;
                break;
            }
        }
        if (already)
            continue;
        if (seen_count < MAX_DIR_IMAGES)
            snprintf(seen[seen_count++], sizeof(seen[0]), "%s", title_id);

        reclaim_stale_nullfs_for_title(title_id);
    }
}

static void log_system_ex_nullfs_mounts(void) {
    struct statfs *mntbuf = NULL;
    int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);

    if (mntcount <= 0 || !mntbuf) {
        di_logf("nullfs mounts at /system_ex/app: none");
        return;
    }

    int logged = 0;
    for (int i = 0; i < mntcount; i++) {
        const char *on = mntbuf[i].f_mntonname;
        char eboot[MAX_PATH];

        if (strncmp(on, "/system_ex/app/", 15) != 0 ||
            strcmp(mntbuf[i].f_fstypename, "nullfs") != 0)
            continue;

        snprintf(eboot, sizeof(eboot), "%s/eboot.bin", on);
        di_logf("nullfs slot [%d]: %s (eboot=%s)", logged + 1, on,
                access(eboot, F_OK) == 0 ? "yes" : "NO");
        logged++;
    }

    if (logged == 0)
        di_logf("nullfs mounts at /system_ex/app: none");
}

static bool image_needs_pfs_nullfs(const char *path) {
    if (!path || !strstr(path, ".ffpfsc"))
        return false;
    if (strstr(path, ".exfat."))
        return false;
    return true;
}

static int count_planned_pfs_nullfs_installs(const dir_image_t *images,
                                             const game_action_t *actions,
                                             int count) {
    int needed = 0;

    for (int i = 0; i < count; i++) {
        if (actions[i] != GAME_ACTION_INSTALL)
            continue;
        if (image_needs_pfs_nullfs(images[i].path))
            needed++;
    }

    return needed;
}

static bool nullfs_capacity_blocked(int active, int needed) {
    return needed > 0 && active + needed > SYSTEM_EX_NULLFS_LIMIT;
}

static bool pfsc_preflight_system_ex_overlay(const char *title_id, const char *inner_mount) {
    char system_ex_app[MAX_PATH];
    char eboot[MAX_PATH];

    if (!title_id || !inner_mount || !inner_mount[0])
        return false;

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    di_logf("preflight: probe nullfs %s -> %s", inner_mount, system_ex_app);

    if (prepare_system_ex_mount_point(title_id) != 0) {
        di_logf("preflight failed: cannot prepare %s", system_ex_app);
        return false;
    }

    if (!overlay_pfs_staging_to_system_ex(title_id, system_ex_app, inner_mount)) {
        di_logf("preflight failed: overlay blocked for %s", title_id);
        cleanup_system_ex_mount(title_id);
        return false;
    }

    snprintf(eboot, sizeof(eboot), "%s/eboot.bin", system_ex_app);
    if (access(eboot, F_OK) != 0) {
        di_logf("preflight failed: eboot missing at %s", eboot);
        cleanup_system_ex_mount(title_id);
        return false;
    }

    di_logf("preflight ok for %s", title_id);
    cleanup_system_ex_mount(title_id);
    return true;
}

static bool mount_pfsc_pfs_to_system_ex(const char *title_id, const char *system_ex_app,
                                        const char *stage_dir, mount_state_t *mounts,
                                        game_action_t action) {
    const char *staging = mounts->pfsc.inner_mount;
    char eboot_staging[MAX_PATH];

    if (!mounts->pfsc.inner_mount[0]) {
        di_logf("inner PFS staging missing for %s", title_id);
        return false;
    }

    staging = mounts->pfsc.inner_mount;
    snprintf(eboot_staging, sizeof(eboot_staging), "%s/eboot.bin", staging);
    if (access(eboot_staging, F_OK) != 0) {
        di_logf("eboot.bin missing at %s", eboot_staging);
        notify("Missing eboot.bin for %s", title_id);
        return false;
    }

    if (mounts->pfsc.inner_is_lvd_pfs) {
        di_logf("LVD overlay for %s from %s", title_id, staging);
        if (overlay_pfs_staging_to_system_ex(title_id, system_ex_app, staging))
            return verify_system_ex_eboot(title_id, system_ex_app);
    }

    if (!mounts->pfsc.inner_is_save_pfs)
        remount_pfsc_inner_pfs_via_save_data(&mounts->pfsc, stage_dir);

    if (mounts->pfsc.inner_is_save_pfs) {
        di_logf("using save-data inner PFS at %s for nullfs", staging);
        if (mount_title_nullfs(title_id, staging))
            return verify_system_ex_eboot(title_id, system_ex_app);
    } else if (mounts->pfsc.inner_is_lvd_pfs) {
        di_logf("using LVD inner PFS at %s for nullfs overlay", staging);
        if (overlay_pfs_staging_to_system_ex(title_id, system_ex_app, staging))
            return verify_system_ex_eboot(title_id, system_ex_app);
    }

    notify("PFS overlay failed for %s (lvd=%d save=%d)", title_id,
           mounts->pfsc.inner_is_lvd_pfs, mounts->pfsc.inner_is_save_pfs);
    cleanup_system_ex_mount(title_id);
    return false;
}

static bool mount_game_to_system_ex(const char *title_id, char *system_ex_app,
                                    const char *stage_dir, const char *nullfs_src,
                                    const char *image_file, mount_state_t *mounts,
                                    bool has_image, game_action_t action) {
    if (has_image && mounts->is_pfsc &&
        (mounts->pfsc.inner_is_save_pfs || mounts->pfsc.inner_is_lvd_pfs)) {
        if (!mount_pfsc_pfs_to_system_ex(title_id, system_ex_app, stage_dir, mounts, action))
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
    int register_count = 0;

    if (staged_count <= 0)
        return 0;

    for (i = 0; i < staged_count; i++) {
        if (staged[i].needs_register)
            register_count++;
    }

    if (register_count <= 0) {
        di_logf("batch register: nothing new to register");
        return 0;
    }

    if (batch_mode && register_count > 1) {
        di_logf("batch register: AppInstallAll for %d new title(s)", register_count);
        install_ret = sceAppInstUtilAppInstallAll(0);
        if (install_ret == 0) {
            di_logf("AppInstallAll ok for %d title(s)", register_count);
            return 0;
        }
        di_logf("AppInstallAll failed: 0x%08x, trying TitleDir per title", install_ret);
    }

    for (i = 0; i < staged_count; i++) {
        if (!staged[i].needs_register)
            continue;

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

static int register_and_finalize_one(staged_install_t *staged) {
    int reg_ret;

    if (!staged || !staged->title_id[0])
        return 0;

    reg_ret = register_staged_titles(staged, 1, false);
    if (reg_ret != 0) {
        notify("Register failed for %s: 0x%08x", staged->title_id, reg_ret);
        di_logf("register failed for %s: 0x%08x", staged->title_id, reg_ret);
        return reg_ret;
    }

    if (staged->needs_register)
        sleep(2);

    finalize_staged_installs(staged, 1);
    if (staged->needs_register) {
        notify_game_installed(staged->title_id);
        browser_log("Installed %s\n", staged->title_id);
    } else {
        browser_log("Remounted %s\n", staged->title_id);
    }
    di_logf("registered and finalized %s", staged->title_id);
    return 0;
}

static void refresh_title_metadata(const char *title_id);

static bool repair_live_game_links(const char *title_id, const char *image_path) {
    char system_ex_app[MAX_PATH];
    char src_sce_sys[MAX_PATH];
    char mount_lnk_path[MAX_PATH];
    char mount_img_lnk_path[MAX_PATH];
    char normalized_image[MAX_PATH];

    if (!title_id || !title_id[0] || !image_path || !image_path[0])
        return false;

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", system_ex_app);
    if (access(src_sce_sys, F_OK) != 0) {
        di_logf("link repair: sce_sys missing at %s", src_sce_sys);
        return false;
    }

    browser_log("Remounting %s...\n", title_id);
    di_logf("link repair for live %s (keeping system_ex mount)", title_id);
    stage_user_app_assets(title_id, src_sce_sys);

    snprintf(mount_lnk_path, sizeof(mount_lnk_path), "/user/app/%s/mount.lnk", title_id);
    write_link_file(mount_lnk_path, system_ex_app);
    di_logf("mount.lnk -> %s", system_ex_app);

    normalize_image_path(image_path, normalized_image, sizeof(normalized_image));
    snprintf(mount_img_lnk_path, sizeof(mount_img_lnk_path),
             "/user/app/%s/mount_img.lnk", title_id);
    write_link_file(mount_img_lnk_path, normalized_image);
    di_logf("mount_img.lnk -> %s", normalized_image);

    refresh_title_metadata(title_id);

    if (!game_library_entry_exists(title_id)) {
        int reg = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);

        di_logf("library register %s: 0x%08x", title_id, reg);
        if (reg != 0)
            notify("Library register %s: 0x%08x", title_id, reg);
    }

    browser_log("Remounted %s\n", title_id);
    return true;
}

static void refresh_title_metadata(const char *title_id) {
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

static int install_one(const char *cwd, const dir_image_t *image, bool defer_register,
                       staged_install_t *staged_out, game_action_t planned_action) {
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
        title_id_for_image_path(image_file, title_id, sizeof(title_id)) &&
        planned_action == GAME_ACTION_SKIP) {
        di_logf("skipping %s: already installed", title_id);
        browser_log("Skipping %s (already installed)\n", title_id);
        return 1;
    }

    if (has_image) {
        char prep_id[32] = {};

        di_logf("image=%s type=%d", image_file, (int)type);

        if (planned_action == GAME_ACTION_REMOUNT &&
            title_id_from_image_path(image_file, prep_id, sizeof(prep_id)) &&
            game_is_live_at_system_ex(prep_id) &&
            repair_live_game_links(prep_id, image_file)) {
            return 0;
        }

        if (planned_action == GAME_ACTION_INSTALL &&
            title_id_from_image_path(image_file, prep_id, sizeof(prep_id))) {
            if (system_ex_has_stale_nullfs(prep_id)) {
                di_logf("clearing stale nullfs mount for %s", prep_id);
                cleanup_system_ex_mount(prep_id);
                purge_title_install_files(prep_id);
            } else if (game_is_live_at_system_ex(prep_id) &&
                       !game_library_entry_exists(prep_id) &&
                       !mount_img_matches_dump(prep_id, image_file)) {
                di_logf("clearing orphan system_ex mount for %s", prep_id);
                cleanup_system_ex_mount(prep_id);
                purge_title_install_files(prep_id);
            }
        }

        if (planned_action != GAME_ACTION_SKIP &&
            (type == IMAGE_TYPE_PFSC || strstr(image_file, ".ffpfsc")))
            cleanup_imgmnt_for_image(image_file);

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

    di_logf("title_id=%s planned_action=%d", title_id, (int)planned_action);

    if (planned_action == GAME_ACTION_INSTALL && has_image &&
        mounts.is_pfsc &&
        (mounts.pfsc.inner_is_lvd_pfs || mounts.pfsc.inner_is_save_pfs) &&
        !pfsc_preflight_system_ex_overlay(title_id, mounts.pfsc.inner_mount)) {
        int active = count_system_ex_nullfs_mounts();

        di_logf("preflight blocked install for %s (active_nullfs=%d)", title_id, active);
        log_system_ex_nullfs_mounts();
        if (active >= SYSTEM_EX_NULLFS_LIMIT)
            notify("%s: PS5 nullfs limit (%d/%d). Reboot, then run installer on all games.",
                   title_id, active, SYSTEM_EX_NULLFS_LIMIT);
        else if (active > 0)
            notify("%s: PFS overlay blocked (%d/%d mounted). Reboot, then reinstall.",
                   title_id, active, SYSTEM_EX_NULLFS_LIMIT);
        else
            notify("%s: PFS-inner overlay not supported on this dump", title_id);

        cleanup_mount_state(&mounts);
        cleanup_stage_dir(stage_dir);
        return -1;
    }

    if (planned_action == GAME_ACTION_INSTALL && !game_is_live(title_id)) {
        di_logf("pre-mount purge for %s (preflight passed, not live)", title_id);
        purge_title_install_files(title_id);
    }

    if (planned_action == GAME_ACTION_SKIP) {
        di_logf("skipping %s: already installed (verified from sce_sys)", title_id);
        browser_log("Skipping %s (already installed)\n", title_id);
        cleanup_mount_state(&mounts);
        cleanup_stage_dir(stage_dir);
        return 1;
    }

    if (planned_action == GAME_ACTION_REMOUNT)
        browser_log("Remounting %s...\n", title_id);
    else
        browser_log("Installing %s...\n", title_id);

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    di_logf("system_ex_app=%s", system_ex_app);

    prepare_game_action(title_id, planned_action);

    if (planned_action == GAME_ACTION_INSTALL)
        prepare_system_ex_mount_point(title_id);

    if (access(nullfs_src, F_OK) != 0) {
        notify("Install source missing for %s: %s", title_id, nullfs_src);
        cleanup_mount_state(&mounts);
        cleanup_stage_dir(stage_dir);
        return -1;
    }

    if (!mount_game_to_system_ex(title_id, system_ex_app, stage_dir, nullfs_src, image_file,
                                 &mounts, has_image, planned_action)) {
        di_logf("mount_game_to_system_ex failed for %s", title_id);
        cleanup_system_ex_mount(title_id);
        cleanup_mount_state(&mounts);
        cleanup_stage_dir(stage_dir);
        return -1;
    }
    di_logf("mounted game content at %s", system_ex_app);
    browser_log("Mounted %s\n", title_id);

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
            char normalized_image[MAX_PATH];

            normalize_image_path(image_file, normalized_image, sizeof(normalized_image));
            snprintf(mount_img_lnk_path, sizeof(mount_img_lnk_path),
                     "/user/app/%s/mount_img.lnk", title_id);
            write_link_file(mount_img_lnk_path, normalized_image);
            di_logf("mount_img.lnk -> %s", normalized_image);
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
            staged_out->needs_register = (planned_action == GAME_ACTION_INSTALL);
        }
        di_logf("staged %s (register=%d)", title_id,
                staged_out ? staged_out->needs_register : 0);
        return 0;
    }

    if (planned_action == GAME_ACTION_REMOUNT) {
        refresh_title_metadata(title_id);
        browser_log("Remounted %s\n", title_id);
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
    refresh_title_metadata(title_id);
    notify_game_installed(title_id);
    browser_log("Installed %s\n", title_id);
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

static bool is_homebrew_app_dir(const char *path) {
    const char *slash;

    if (!path || !path[0])
        return false;

    slash = strrchr(path, '/');
    return slash && !strcmp(slash + 1, "dump_installer");
}

static int run_install_from_directory(const char *cwd) {
    dir_image_t images[MAX_DIR_IMAGES];
    game_action_t planned_actions[MAX_DIR_IMAGES];
    int image_count = list_images_in_dir(cwd, images, MAX_DIR_IMAGES);
    int skip_count = 0;
    int remount_count = 0;
    int install_count = 0;
    bool partial_batch = false;

    di_logf("found %d installable item(s) in cwd", image_count);
    for (int i = 0; i < image_count; i++) {
        di_logf("  [%d] %s (type=%d)", i, images[i].path, (int)images[i].type);
    }

    if (image_count > 0) {
        partial_batch = image_count > 1 && batch_is_partial(images, image_count);
        di_logf("partial_batch=%d", partial_batch);

        for (int i = 0; i < image_count; i++) {
            char title_id[32] = {};

            planned_actions[i] = GAME_ACTION_INSTALL;
            if (title_id_for_image_path(images[i].path, title_id, sizeof(title_id))) {
                di_log_game_diagnostics("plan", title_id, images[i].path);
                planned_actions[i] = plan_game_action(title_id, images[i].path, partial_batch);
            }

            if (planned_actions[i] == GAME_ACTION_SKIP)
                skip_count++;
            else if (planned_actions[i] == GAME_ACTION_REMOUNT)
                remount_count++;
            else
                install_count++;
        }

        if (image_count > 1) {
            sort_batch_by_action(images, planned_actions, image_count);
            di_logf("batch order (install first, pfs-inner before exfat):");
            for (int i = 0; i < image_count; i++)
                di_logf("  [%d] %s action=%d", i, images[i].path,
                        (int)planned_actions[i]);
        }

        di_logf("batch plan: skip=%d remount=%d install=%d",
                skip_count, remount_count, install_count);

        if (skip_count == image_count) {
            char done_msg[64];

            snprintf(done_msg, sizeof(done_msg), "All %d games already installed",
                     image_count);
            browser_log("%s\n", done_msg);
            if (image_count > 1)
                notify_installer_toast(done_msg);
            di_logf("all %d game(s) already live", image_count);
            return 0;
        }

        for (int i = 0; i < image_count; i++) {
            char orphan_id[32] = {};

            if (planned_actions[i] != GAME_ACTION_INSTALL)
                continue;
            if (!title_id_for_image_path(images[i].path, orphan_id, sizeof(orphan_id)))
                continue;
            if (!game_is_live_at_system_ex(orphan_id))
                continue;
            if (game_library_entry_exists(orphan_id))
                continue;
            if (mount_img_matches_dump(orphan_id, images[i].path))
                continue;

            di_logf("batch pre-purge orphan mount %s", orphan_id);
            cleanup_system_ex_mount(orphan_id);
            purge_title_install_files(orphan_id);
        }

        cleanup_stale_system_ex_nullfs_mounts();

        {
            int active_nullfs = count_system_ex_nullfs_mounts();
            int pfs_needed = count_planned_pfs_nullfs_installs(images, planned_actions,
                                                               image_count);

            di_logf("nullfs budget: active=%d need=%d limit=%d",
                    active_nullfs, pfs_needed, SYSTEM_EX_NULLFS_LIMIT);
            log_system_ex_nullfs_mounts();

            if (nullfs_capacity_blocked(active_nullfs, pfs_needed)) {
                notify("PFS install needs reboot (%d/%d nullfs slots used). "
                       "Reboot, then run installer — all games will install in one pass.",
                       active_nullfs, SYSTEM_EX_NULLFS_LIMIT);
            }
        }

        if (skip_count == 0 && remount_count == 0 && install_count > 0) {
            if (remount_system_ex() == 0)
                di_logf("remount_system_ex done");
            cleanup_all_imgmnt_staging();
        } else {
            di_logf("keeping live mounts (skip=%d remount=%d), no remount_system_ex",
                    skip_count, remount_count);
        }

        if (install_count > 0 && skip_count > 0)
            browser_log("Installing %d, skipping %d already live...\n",
                          install_count, skip_count);
        else if (remount_count > 0 && install_count == 0)
            browser_log("Remounting %d games...\n", remount_count);
        else if (install_count > 0 && remount_count > 0)
            browser_log("Installing %d, remounting %d...\n", install_count, remount_count);
        else if (install_count > 0)
            browser_log("Installing %d games...\n", install_count);
    } else if (remount_system_ex() == 0) {
        di_logf("remount_system_ex done");
    }

    mkdir("/system_ex/app", 0777);

    sceAppInstUtilInitialize();

    if (image_count > 0) {
        bool batch_mode = image_count > 1;
        staged_install_t staged[MAX_DIR_IMAGES];
        int staged_count = 0;
        int failures = 0;
        int skipped = 0;
        int installed = 0;

        if (batch_mode)
            browser_log("Found %d images\n", image_count);

        for (int i = 0; i < image_count; i++) {
            char early_id[32] = {};
            game_action_t action = planned_actions[i];

            di_logf("batch item %d/%d", i + 1, image_count);
            if (title_id_for_image_path(images[i].path, early_id, sizeof(early_id))) {
                di_log_game_diagnostics("batch", early_id, images[i].path);
                di_logf("batch action %s: %d (0=skip 1=remount 2=install)",
                        early_id, (int)action);
            }

            if (action == GAME_ACTION_SKIP) {
                di_logf("batch skip %s: already installed", early_id);
                browser_log("Skipping %s (already installed)\n", early_id);
                skipped++;
                continue;
            }

            if (action == GAME_ACTION_INSTALL &&
                image_needs_pfs_nullfs(images[i].path)) {
                if (early_id[0])
                    reclaim_stale_nullfs_for_title(early_id);

                int active_nullfs = count_system_ex_nullfs_mounts();

                if (active_nullfs >= SYSTEM_EX_NULLFS_LIMIT) {
                    di_logf("nullfs slots full (%d/%d), skipping staging for %s",
                            active_nullfs, SYSTEM_EX_NULLFS_LIMIT, early_id);
                    log_system_ex_nullfs_mounts();
                    notify("%s: skipped — reboot first (%d/%d PFS mounts active)",
                           early_id[0] ? early_id : images[i].path,
                           active_nullfs, SYSTEM_EX_NULLFS_LIMIT);
                    failures++;
                    continue;
                }
            }

            staged_install_t *slot = batch_mode && staged_count < MAX_DIR_IMAGES ?
                                     &staged[staged_count] : NULL;
            int install_result = install_one(cwd, &images[i], batch_mode, slot, action);
            if (install_result < 0)
                failures++;
            else if (install_result > 0)
                skipped++;
            else {
                installed++;
                if (batch_mode && slot) {
                    staged_count++;
                    if (register_and_finalize_one(slot) != 0)
                        failures++;
                }
            }
        }

        if (failures > 0) {
            di_logf("batch finished: %d installed, %d skipped, %d failed of %d",
                    installed, skipped, failures, image_count);
            if (installed > 0)
                notify("Installed %d, %d failed", installed, failures);
            else
                notify("%d of %d installs failed", failures, image_count);
            return installed > 0 ? 0 : -1;
        }

        if (skipped > 0 && installed > 0) {
            char done_msg[80];

            snprintf(done_msg, sizeof(done_msg),
                     "Installed %d new, skipped %d already installed",
                     installed, skipped);
            browser_log("Done — %s\n", done_msg);
            if (image_count > 1)
                notify_installer_toast(done_msg);
        } else if (skipped > 0 && installed == 0) {
            char done_msg[64];

            snprintf(done_msg, sizeof(done_msg), "Remounted %d, skipped %d",
                     image_count - skipped, skipped);
            browser_log("Done — %s\n", done_msg);
            if (image_count > 1)
                notify_installer_toast(done_msg);
        } else if (image_count > 1) {
            char done_msg[48];

            snprintf(done_msg, sizeof(done_msg), "%d games ready", image_count);
            browser_log("Done — %s\n", done_msg);
            notify_installer_toast(done_msg);
        }

        return 0;
    }

    return install_one(cwd, NULL, false, NULL, GAME_ACTION_INSTALL);
}

static int run_autoload_mode(void) {
    char locations[48][MAX_PATH];
    int location_count = autoscan_build_locations(locations, 48);
    int processed = 0;
    int last_ret = 0;

    browser_log("Scanning storage for games...\n");

    for (int i = 0; i < location_count; i++) {
        int count = autoscan_count_installables(locations[i]);

        if (count <= 0)
            continue;

        browser_log("Found %d installable item(s) at %s\n", count, locations[i]);
        last_ret = run_install_from_directory(locations[i]);
        processed++;
    }

    if (processed == 0) {
        browser_log("No installable games found on scanned storage\n");
        return 0;
    }

    return last_ret;
}

int main(int argc, char **argv) {
    char cwd[PATH_MAX];
    int ret = 0;

    if (argc >= 2 && !strcmp(argv[1], "--cleanup-dilocs")) {
        cleanup_legacy_dilocs();
        notify("Removed /data/dilocs only");
        ret = 0;
        goto done;
    }

    if (argc >= 2 && !strcmp(argv[1], "--purge-grey")) {
        notify("Purging grey library entries...");
        if (!getcwd(cwd, sizeof(cwd))) {
            notify("Unable to determine working directory");
            ret = -1;
            goto done;
        }
        ret = purge_grey_titles_in_dir(cwd);
        notify("Run install again after grey icons disappear");
        goto done;
    }

    if (argc >= 2 && !strcmp(argv[1], "--autoload")) {
        if (ensure_mount_dirs() != 0) {
            di_logf("ensure_mount_dirs failed");
            ret = 1;
            goto done;
        }
        browser_log("Dump Installer autoload\n");
        ret = run_autoload_mode();
        goto done;
    }

    if (!getcwd(cwd, sizeof(cwd))) {
        di_logf("getcwd failed: errno=%d (%s)", errno, strerror(errno));
        printf("Error: Unable to determine working directory\n");
        ret = -1;
        goto done;
    }

    di_logf("working directory: %s", cwd);

    if (ensure_mount_dirs() != 0) {
        di_logf("ensure_mount_dirs failed");
        ret = 1;
        goto done;
    }

    if (!strcmp(cwd, "/") || !strcmp(cwd, "//")) {
        if (autoscan_any_games_found()) {
            browser_log("Dump Installer autoload (payload launch)\n");
            ret = run_autoload_mode();
            goto done;
        }
        notify("Select a folder with games, not console root");
        di_logf("refusing install from console root (cwd=%s)", cwd);
        ret = -1;
        goto done;
    }

    browser_log("Dump Installer — %s\n", cwd);

    if (is_homebrew_app_dir(cwd)) {
        if (autoscan_any_games_found()) {
            browser_log("Scanning storage...\n");
            ret = run_autoload_mode();
        } else {
            browser_log("No installable games found on scanned storage\n");
            ret = 0;
        }
        goto done;
    }

    {
        dir_image_t probe[MAX_DIR_IMAGES];

        if (list_images_in_dir(cwd, probe, MAX_DIR_IMAGES) == 0 &&
            !autoscan_dir_has_games(cwd)) {
            if (autoscan_any_games_found()) {
                browser_log("No games here — scanning storage...\n");
                ret = run_autoload_mode();
                goto done;
            }
            browser_log("No installable games found\n");
            ret = 0;
            goto done;
        }
    }

    ret = run_install_from_directory(cwd);

done:
    return ret;
}

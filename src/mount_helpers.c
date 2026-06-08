#include <sys/uio.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <unistd.h>

#include "mount_helpers.h"
#include "hash.h"
#include "log.h"
#include "utils.h"
#include "types.h"

int prepare_system_ex_mount_point(const char *title_id);

int remount_system_ex(void) {
    struct iovec iov[] = {
        IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
        IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
        IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
    };
    return nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE);
}

int mount_nullfs(const char* src, const char* dst) {
    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"), IOVEC_ENTRY("nullfs"),
        IOVEC_ENTRY("from"),   IOVEC_ENTRY(src),
        IOVEC_ENTRY("fspath"), IOVEC_ENTRY(dst),
    };
    return nmount(iov, IOVEC_SIZE(iov), 0);
}

bool mount_title_nullfs(const char *title_id, const char *src_path) {
    char dst[MAX_PATH];
    char src_eboot[MAX_PATH];
    char dst_eboot[MAX_PATH];

    if (!title_id || !src_path || !*title_id || !*src_path)
        return false;

    snprintf(dst, sizeof(dst), "/system_ex/app/%s", title_id);
    snprintf(src_eboot, sizeof(src_eboot), "%s/eboot.bin", src_path);
    snprintf(dst_eboot, sizeof(dst_eboot), "%s/eboot.bin", dst);

    if (access(src_path, F_OK) != 0) {
        di_logf("nullfs src missing for %s: %s", title_id, src_path);
        return false;
    }
    if (access(src_eboot, F_OK) != 0) {
        di_logf("nullfs src eboot missing for %s: %s", title_id, src_eboot);
        return false;
    }

    if (is_mounted(dst) && access(dst_eboot, F_OK) == 0) {
        di_logf("nullfs already active for %s: %s -> %s", title_id, src_path, dst);
        return true;
    }

    if (prepare_system_ex_for_nullfs(title_id) != 0)
        return false;

    di_logf("nullfs mount: %s -> %s", src_path, dst);
    if (mount_nullfs(src_path, dst) != 0) {
        di_logf("nullfs mount failed: %s -> %s errno=%d (%s)",
                src_path, dst, errno, strerror(errno));
        return false;
    }

    if (access(dst_eboot, F_OK) != 0) {
        di_logf("nullfs mounted but eboot missing at %s, rolling back", dst_eboot);
        unmount(dst, MNT_FORCE);
        return false;
    }

    di_logf("nullfs overlay ok: %s -> %s", src_path, dst);
    return true;
}

static int mount_unionfs_overlay(const char *src, const char *dst) {
    struct iovec iov[] = {
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("unionfs"),
        IOVEC_ENTRY("from"),      IOVEC_ENTRY(src),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY(dst),
        IOVEC_ENTRY("copymode"),  IOVEC_ENTRY("transparent"),
        IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("fnodup"),    IOVEC_ENTRY(NULL),
    };
    return nmount(iov, IOVEC_SIZE(iov), MNT_RDONLY);
}

bool mount_overlay_to_system_ex(const char *src, const char *dst) {
    if (!src || !dst || !*src || !*dst)
        return false;

    if (mount_nullfs(src, dst) == 0) {
        di_logf("nullfs overlay ok: %s -> %s", src, dst);
        return true;
    }

    di_logf("nullfs overlay failed: %s -> %s errno=%d (%s)", src, dst, errno, strerror(errno));

    if (mount_unionfs_overlay(src, dst) == 0) {
        di_logf("unionfs overlay ok: %s -> %s", src, dst);
        return true;
    }

    di_logf("unionfs overlay failed: %s -> %s errno=%d (%s)", src, dst, errno, strerror(errno));
    return false;
}

static bool path_is_mount_point(const char *path, struct statfs *sfs_out) {
    struct statfs sfs;

    if (!path || statfs(path, &sfs) != 0)
        return false;
    if (strcmp(sfs.f_mntonname, path) != 0)
        return false;
    if (sfs_out)
        *sfs_out = sfs;
    return true;
}

static bool unmount_all_layers_at(const char *path) {
    for (int i = 0; i < 32; i++) {
        struct statfs sfs;

        if (!path_is_mount_point(path, &sfs))
            return true;

        di_logf("unmounting %s (%s)", path, sfs.f_fstypename);
        if (unmount(path, MNT_FORCE) != 0 && errno != ENOENT && errno != EINVAL) {
            di_logf("unmount %s (%s) failed: %s", path, sfs.f_fstypename, strerror(errno));
            return false;
        }
    }

    di_logf("unmount layer limit reached for %s", path);
    return false;
}

static bool clear_mount_path(const char *path) {
    if (!path || !*path)
        return false;

    if (!unmount_all_layers_at(path))
        return false;

    struct stat st;
    if (stat(path, &st) == 0) {
        if (S_ISDIR(st.st_mode))
            remove_dir(path);
        else
            unlink(path);
    }

    if (path_is_mount_point(path, NULL))
        return false;

    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

bool mount_overlay_via_bridge(const char *src, const char *dst, const char *bridge) {
    if (!src || !dst || !bridge || !*bridge)
        return false;

    if (!clear_mount_path(bridge)) {
        di_logf("failed to prepare bridge path %s", bridge);
        return false;
    }

    if (mount_nullfs(src, bridge) != 0) {
        di_logf("bridge step1 failed: %s -> %s errno=%d (%s)",
                src, bridge, errno, strerror(errno));
        return false;
    }

    di_logf("bridge step1 ok: %s -> %s", src, bridge);

    if (strncmp(dst, "/system_ex/app/", 15) == 0) {
        const char *title_id = dst + 15;
        if (prepare_system_ex_for_nullfs(title_id) != 0) {
            di_logf("failed to prepare bridge dst %s", dst);
            unmount(bridge, MNT_FORCE);
            rmdir(bridge);
            return false;
        }
    } else if (!clear_mount_path(dst)) {
        di_logf("failed to prepare bridge dst %s", dst);
        unmount(bridge, MNT_FORCE);
        rmdir(bridge);
        return false;
    }

    if (mount_nullfs(bridge, dst) != 0) {
        di_logf("bridge step2 failed: %s -> %s errno=%d (%s)",
                bridge, dst, errno, strerror(errno));
        unmount(bridge, MNT_FORCE);
        rmdir(bridge);
        return false;
    }

    di_logf("bridge step2 ok: %s -> %s", bridge, dst);
    return true;
}

int is_mounted(const char* path) {
    struct statfs sfs;
    if (!path_is_mount_point(path, &sfs))
        return 0;
    return strcmp(sfs.f_fstypename, "nullfs") == 0;
}

int prepare_system_ex_title_reinstall(const char *title_id) {
    if (!title_id || !*title_id)
        return -1;

    di_logf("reinstall prep for /system_ex/app/%s", title_id);
    return prepare_system_ex_mount_point(title_id);
}

int prepare_system_ex_for_nullfs(const char *title_id) {
    di_logf("prepare /system_ex/app/%s for nullfs overlay", title_id);
    return prepare_system_ex_mount_point(title_id);
}

static bool mount_path_is_protected(const char *on, const char *const *protected_paths,
                                    int protected_count) {
    if (!on || !protected_paths || protected_count <= 0)
        return false;

    for (int i = 0; i < protected_count; i++) {
        const char *prot = protected_paths[i];

        if (!prot || !prot[0])
            continue;
        if (strcmp(on, prot) == 0)
            return true;

        size_t plen = strlen(prot);
        if (strncmp(on, prot, plen) == 0 && on[plen] == '/')
            return true;
    }

    return false;
}

void reset_installer_session_mounts_excluding(const char *const *protected_paths,
                                              int protected_count) {
    struct statfs *mntbuf = NULL;
    int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);

    di_logf("session mount reset start (protect %d path(s))", protected_count);
    if (mntcount <= 0 || !mntbuf) {
        di_logf("session mount reset: no mount table");
        remount_system_ex();
        return;
    }

    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < mntcount; i++) {
            const char *on = mntbuf[i].f_mntonname;

            if (pass == 0) {
                if (strncmp(on, "/system_ex/app/", 15) != 0)
                    continue;
            } else if (pass == 1) {
                if (strncmp(on, "/data/imgmnt/", 15) != 0)
                    continue;
            } else {
                if (strncmp(on, "/data/imgmnt/bridgemnt/", 23) != 0 &&
                    strncmp(on, "/data/imgmnt/dipayload/", 23) != 0)
                    continue;
            }

            if (!path_is_mount_point(on, NULL))
                continue;
            if (mount_path_is_protected(on, protected_paths, protected_count)) {
                di_logf("session reset: keep protected %s", on);
                continue;
            }

            di_logf("session reset: unmount %s (%s)", on, mntbuf[i].f_fstypename);
            unmount(on, MNT_FORCE);
        }
    }

    remount_system_ex();
    di_logf("session mount reset done");
}

void reset_installer_session_mounts(void) {
    reset_installer_session_mounts_excluding(NULL, 0);
}

static bool imgmnt_path_matches_image_hash(const char *mount_path, const char *hash_suffix) {
    const char *hit;

    if (!mount_path || !hash_suffix || !hash_suffix[0])
        return false;

    hit = strstr(mount_path, hash_suffix);
    if (!hit)
        return false;

    return hit[8] == '\0' || hit[8] == '/' || hit[8] == '_';
}

void cleanup_imgmnt_for_image(const char *image_path) {
    char hash_suffix[16];
    struct statfs *mntbuf = NULL;
    int mntcount;

    if (!image_path || !image_path[0])
        return;

    char pfscache_pfs[MAX_PATH];

    snprintf(hash_suffix, sizeof(hash_suffix), "%08x", fnv1a32(image_path));
    di_logf("imgmnt cleanup for %s (hash %s)", image_path, hash_suffix);

    snprintf(pfscache_pfs, sizeof(pfscache_pfs), "/data/imgmnt/pfscache/%s_pfs_image.dat",
             hash_suffix);
    if (access(pfscache_pfs, F_OK) == 0) {
        di_logf("imgmnt cleanup remove pfscache %s", pfscache_pfs);
        unlink(pfscache_pfs);
    }

    for (int pass = 0; pass < 2; pass++) {
        mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);
        if (mntcount <= 0 || !mntbuf)
            break;

        for (int i = 0; i < mntcount; i++) {
            const char *on = mntbuf[i].f_mntonname;

            if (pass == 0) {
                if (strncmp(on, "/data/imgmnt/pfsmnt/", 20) != 0 &&
                    strncmp(on, "/data/imgmnt/exfatmnt/", 22) != 0 &&
                    strncmp(on, "/data/imgmnt/ufsmnt/", 21) != 0)
                    continue;
            } else if (strncmp(on, "/data/imgmnt/pfscmnt/", 21) != 0) {
                continue;
            }

            if (!imgmnt_path_matches_image_hash(on, hash_suffix))
                continue;
            if (!path_is_mount_point(on, NULL))
                continue;

            di_logf("imgmnt cleanup unmount %s (%s)", on, mntbuf[i].f_fstypename);
            unmount(on, MNT_FORCE);
        }
    }
}

void cleanup_all_imgmnt_staging(void) {
    struct statfs *mntbuf = NULL;
    int mntcount;

    di_logf("imgmnt cleanup: all staging mounts");
    for (int pass = 0; pass < 3; pass++) {
        mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);
        if (mntcount <= 0 || !mntbuf)
            break;

        for (int i = 0; i < mntcount; i++) {
            const char *on = mntbuf[i].f_mntonname;

            if (strncmp(on, "/data/imgmnt/", 15) != 0)
                continue;

            if (pass == 0) {
                if (strncmp(on, "/system_ex/", 11) == 0)
                    continue;
                if (strncmp(on, "/data/imgmnt/bridgemnt/", 23) != 0 &&
                    strncmp(on, "/data/imgmnt/dipayload/", 23) != 0)
                    continue;
            } else if (pass == 1) {
                if (strncmp(on, "/data/imgmnt/pfsmnt/", 20) != 0 &&
                    strncmp(on, "/data/imgmnt/pfscmnt/", 21) != 0 &&
                    strncmp(on, "/data/imgmnt/exfatmnt/", 22) != 0 &&
                    strncmp(on, "/data/imgmnt/ufsmnt/", 21) != 0)
                    continue;
            }

            if (!path_is_mount_point(on, NULL))
                continue;

            di_logf("imgmnt cleanup unmount %s (%s)", on, mntbuf[i].f_fstypename);
            unmount(on, MNT_FORCE);
        }
    }
}

void recover_system_ex_overlay_mounts(void) {
    struct statfs *mntbuf = NULL;
    int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);

    di_logf("system_ex overlay recovery start");
    if (mntcount <= 0 || !mntbuf) {
        di_logf("system_ex overlay recovery: no mount table");
        return;
    }

    for (int i = 0; i < mntcount; i++) {
        const char *on = mntbuf[i].f_mntonname;
        const char *fst = mntbuf[i].f_fstypename;

        if (strncmp(on, "/system_ex/app/", 15) != 0)
            continue;
        if (strcmp(fst, "nullfs") != 0 && strcmp(fst, "unionfs") != 0)
            continue;

        di_logf("recovery: unmount stale %s at %s", fst, on);
        unmount(on, MNT_FORCE);
    }

    remount_system_ex();
    di_logf("system_ex overlay recovery done");
}

int prepare_system_ex_mount_point(const char *title_id) {
    char path[MAX_PATH];
    struct stat pre;

    snprintf(path, sizeof(path), "/system_ex/app/%s", title_id);

    if (stat(path, &pre) == 0) {
        di_logf("prepare %s: clearing existing path (mode=%o)", path, pre.st_mode);
    } else {
        di_logf("prepare %s: creating fresh mount point", path);
    }

    if (!unmount_all_layers_at(path)) {
        di_logf("failed to clear mount layers at %s", path);
        return -1;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (remove_dir(path) != 0) {
                    di_logf("remove_dir %s failed (attempt %d), retrying unmount",
                            path, attempt + 1);
                    if (!unmount_all_layers_at(path))
                        return -1;
                    continue;
                }
            } else {
                unlink(path);
            }
        }
        break;
    }

    if (path_is_mount_point(path, NULL)) {
        di_logf("mount point still active after cleanup: %s", path);
        return -1;
    }

    if (mkdir(path, 0755) != 0 && errno != EEXIST)
        return -1;

    return 0;
}

bool mount_fstype_at_path(const char *fstype, const char *path) {
    struct statfs sfs;
    if (!fstype || !path || !path_is_mount_point(path, &sfs))
        return false;
    return strcmp(sfs.f_fstypename, fstype) == 0;
}

#define DILOCS_ROOT "/data/dilocs"
#define DILOCS_ROOT_SLASH "/data/dilocs/"

static bool is_safe_dilocs_path(const char *path) {
    size_t root_len;
    size_t path_len;

    if (!path)
        return false;

    root_len = strlen(DILOCS_ROOT);
    path_len = strlen(path);

    if (path_len < root_len)
        return false;
    if (strncmp(path, DILOCS_ROOT, root_len) != 0)
        return false;
    if (path_len == root_len)
        return true;
    if (path[root_len] != '/')
        return false;
    if (strstr(path, "/../") || strcmp(path + path_len - 3, "/..") == 0)
        return false;

    return true;
}

static bool is_safe_dilocs_entry_name(const char *name) {
    if (!name || !name[0])
        return false;
    if (!strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    if (strchr(name, '/'))
        return false;

    return true;
}

static bool unmount_dilocs_mirror_only(const char *path) {
    struct statfs sfs;

    if (!is_safe_dilocs_path(path))
        return false;

    if (!path_is_mount_point(path, &sfs))
        return true;

    if (strcmp(sfs.f_fstypename, "nullfs") != 0 &&
        strcmp(sfs.f_fstypename, "unionfs") != 0) {
        di_logf("dilocs cleanup skip (not a mirror mount): %s (%s)", path, sfs.f_fstypename);
        return false;
    }

    if (unmount(path, MNT_FORCE) != 0 && errno != ENOENT && errno != EINVAL) {
        di_logf("dilocs cleanup unmount failed: %s (%s)", path, strerror(errno));
        return false;
    }

    di_logf("dilocs cleanup unmounted: %s", path);
    return true;
}

static bool rmdir_empty_dilocs_path(const char *path) {
    if (!is_safe_dilocs_path(path))
        return false;

    if (rmdir(path) == 0) {
        di_logf("dilocs cleanup rmdir ok: %s", path);
        return true;
    }

    if (errno == ENOENT)
        return true;

    di_logf("dilocs cleanup rmdir %s: %s", path, strerror(errno));
    return false;
}

void cleanup_legacy_dilocs(void) {
    struct statfs *mntbuf = NULL;
    int mntcount = getmntinfo(&mntbuf, MNT_NOWAIT);

    di_logf("dilocs cleanup start (only %s)", DILOCS_ROOT);

    if (mntcount > 0 && mntbuf) {
        for (int i = 0; i < mntcount; i++) {
            const char *on = mntbuf[i].f_mntonname;

            if (!is_safe_dilocs_path(on) || strcmp(on, DILOCS_ROOT) == 0)
                continue;

            unmount_dilocs_mirror_only(on);
        }
    }

    DIR *d = opendir(DILOCS_ROOT);
    if (!d) {
        di_logf("dilocs cleanup: %s not present", DILOCS_ROOT);
        return;
    }

    struct dirent *e;
    char entry_path[MAX_PATH];

    while ((e = readdir(d))) {
        if (!is_safe_dilocs_entry_name(e->d_name))
            continue;

        snprintf(entry_path, sizeof(entry_path), "%s/%s", DILOCS_ROOT, e->d_name);
        if (!is_safe_dilocs_path(entry_path))
            continue;

        unmount_dilocs_mirror_only(entry_path);
        rmdir_empty_dilocs_path(entry_path);
    }

    closedir(d);
    rmdir_empty_dilocs_path(DILOCS_ROOT);
    di_logf("dilocs cleanup finished");
}
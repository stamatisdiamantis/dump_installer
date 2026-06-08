#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/mount.h>

#include "types.h"
#include "hash.h"
#include "log.h"
#include "mount_helpers.h"
#include "pfs_mount.h"


bool mount_pfs_save_data_to(const char *file_path, const char *mount_point) {
    if (!file_path || !mount_point || !*mount_point)
        return false;

    if (mkdir(mount_point, 0777) != 0 && errno != EEXIST) {
        notify("mkdir failed: %s", strerror(errno));
        return false;
    }

    MountSaveDataOpt opt;
    sceFsInitMountSaveDataOpt(&opt);
    opt.budgetid = PFS_MOUNT_BUDGET_ID;

    uint8_t key[0x20] = {0};

    int ret = sceFsMountSaveData(&opt, file_path, mount_point, key);
    if (ret < 0) {
        di_logf("save-data PFS mount failed for %s: 0x%08x", file_path, ret);
        rmdir(mount_point);
        return false;
    }

    di_logf("save-data PFS mounted: %s -> %s", file_path, mount_point);
    return true;
}

bool mount_pfs_image(const char* file_path, char* out_mount_point) {
    const char* filename = strrchr(file_path, '/') ? strrchr(file_path, '/') + 1 : file_path;
    char mount_name[MAX_PATH];
    char* dot;

    snprintf(mount_name, sizeof(mount_name), "%s", filename);
    dot = strrchr(mount_name, '.');
    if (dot)
        *dot = '\0';

    snprintf(out_mount_point, MAX_PATH, "/data/imgmnt/pfsmnt/%s_%08x",
             mount_name, fnv1a32(file_path));

    struct stat st;
    if (stat(out_mount_point, &st) == 0) {
        unmount_pfs(out_mount_point);
    }

    if (mkdir(out_mount_point, 0777) && errno != EEXIST) {
        notify("mkdir failed: %s", strerror(errno));
        return false;
    }

    //notify("Mounting PFS: %s → %s", file_path, out_mount_point);

    if (!mount_pfs_save_data_to(file_path, out_mount_point))
        return false;

    return true;
}

void unmount_pfs(const char* mount_point) {
    struct statfs sfs;

    if (!mount_point || !*mount_point)
        return;

    UmountSaveDataOpt opt;
    sceFsInitUmountSaveDataOpt(&opt);
    sceFsUmountSaveData(&opt, mount_point, -1, 1);

    if (statfs(mount_point, &sfs) == 0 && strcmp(sfs.f_fstypename, "pfs") == 0)
        unmount(mount_point, MNT_FORCE);

    rmdir(mount_point);
}
#include "lvd_mount.h"
#include "hash.h"
#include "image.h"
#include "utils.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/uio.h>

static uint16_t pfs_lvd_game_image_type_from_selector(uint8_t selector) {
    static const uint16_t table[8] = {1, 2, 3, 4, 8, 9, 10, 11};
    uint32_t idx = (((uint32_t)selector >> 7) & 1u) + 2u * ((uint32_t)selector & 0x1Fu);
    idx ^= 1u;
    if (idx >= 8u)
        return LVD_ATTACH_IMAGE_TYPE_PFS_SAVE_DATA;
    return table[idx];
}

static uint16_t normalize_lvd_raw_flags(uint16_t raw_flags) {
    if ((raw_flags & 0x800Eu) != 0u) {
        uint32_t raw = (uint32_t)raw_flags;
        uint32_t len = (raw & 0xFFFF8000u) + ((raw & 2u) << 6) +
                       (8u * (raw & 1u)) + (2u * ((raw >> 2) & 1u)) +
                       (2u * (raw & 8u)) + 4u;
        return (uint16_t)len;
    }
    return (uint16_t)(8u * ((uint32_t)raw_flags & 1u) + 4u);
}

static uint16_t get_lvd_image_type(uint8_t nested_selector, bool nested_profile) {
    if (nested_profile)
        return pfs_lvd_game_image_type_from_selector(nested_selector);
    return LVD_ATTACH_IMAGE_TYPE_PFS_SAVE_DATA;
}

static void strip_extension(const char *filename, char *out, size_t out_size) {
    const char *dot = strrchr(filename, '.');
    size_t len = dot ? (size_t)(dot - filename) : strlen(filename);
    if (len >= out_size)
        len = out_size - 1;
    memcpy(out, filename, len);
    out[len] = '\0';
}

static bool wait_for_lvd_device(const char *devname) {
    for (int i = 0; i < LVD_NODE_WAIT_RETRIES; i++) {
        if (access(devname, F_OK) == 0)
            return true;
        usleep(LVD_NODE_WAIT_US);
    }
    notify("LVD device did not appear: %s", devname);
    return false;
}

static void prepare_pfs_mount_dir(const char *mount_point) {
    struct statfs sfs;
    if (statfs(mount_point, &sfs) == 0 && strcmp(sfs.f_fstypename, "pfs") == 0)
        unmount(mount_point, MNT_FORCE);
    rmdir(mount_point);
}

void build_image_mount_point(const char *file_path, image_type_t fs_type,
                             const char *mount_base, char mount_point[MAX_PATH]) {
    const char *filename = strrchr(file_path, '/') ? strrchr(file_path, '/') + 1 : file_path;
    char base_name[MAX_PATH];
    char mount_name[MAX_PATH];

    strip_extension(filename, base_name, sizeof(base_name));
    size_t base_len = strlen(base_name);
    size_t max_base_len = sizeof(mount_name) - 1u - 9u;
    if (base_len > max_base_len)
        base_len = max_base_len;
    memcpy(mount_name, base_name, base_len);
    mount_name[base_len] = '\0';
    snprintf(mount_name + base_len, sizeof(mount_name) - base_len, "_%08x",
             fnv1a32(file_path));

    (void)fs_type;
    snprintf(mount_point, MAX_PATH, "%s/%s", mount_base, mount_name);
}

static bool lvd_attach_pfs_region_ex(const char *file_path, uint64_t offset, uint64_t size,
                                    uint8_t nested_selector, uint32_t sector_size,
                                    int *unit_id_out, char *devname_out, size_t devname_size) {
    if (!file_path || size == 0)
        return false;

    int lvd_fd = open(LVD_CTRL_PATH, O_RDWR);
    if (lvd_fd < 0) {
        notify("open %s failed: %s", LVD_CTRL_PATH, strerror(errno));
        return false;
    }

    bool nested_profile = nested_selector != 0;
    lvd_ioctl_layer_v0_t layers[1];
    memset(layers, 0, sizeof(layers));
    layers[0].source_type = LVD_ENTRY_TYPE_FILE;
    layers[0].flags = LVD_ENTRY_FLAG_NO_BITMAP;
    layers[0].path = file_path;
    layers[0].offset = offset;
    layers[0].size = size;

    lvd_ioctl_attach_v0_t req;
    memset(&req, 0, sizeof(req));
    req.io_version = LVD_ATTACH_IO_VERSION_V0;
    req.device_id = -1;
    req.sector_size = sector_size;
    req.secondary_unit = sector_size;
    req.flags = normalize_lvd_raw_flags(LVD_ATTACH_RAW_FLAGS_SINGLE_RO);
    req.image_type = get_lvd_image_type(nested_selector, nested_profile);
    req.layer_count = 1;
    req.device_size = size;
    req.layers_ptr = layers;

    int ret = ioctl(lvd_fd, SCE_LVD_IOC_ATTACH_V0, &req);
    int saved_errno = ret != 0 ? errno : 0;
    close(lvd_fd);

    if (ret != 0) {
        errno = saved_errno;
        return false;
    }

    if (req.device_id < 0)
        return false;

    snprintf(devname_out, devname_size, "/dev/lvd%d", req.device_id);
    if (!wait_for_lvd_device(devname_out)) {
        lvd_detach(req.device_id);
        return false;
    }

    *unit_id_out = req.device_id;
    return true;
}

bool lvd_attach_pfs(const char *file_path, uint8_t nested_selector, uint32_t sector_size,
                    int *unit_id_out, char *devname_out, size_t devname_size) {
    struct stat st;
    if (stat(file_path, &st) != 0 || st.st_size < 0) {
        notify("LVD stat failed for %s: %s", file_path, strerror(errno));
        return false;
    }

    return lvd_attach_pfs_region_ex(file_path, 0, (uint64_t)st.st_size, nested_selector,
                                    sector_size, unit_id_out, devname_out, devname_size);
}

void lvd_detach(int unit_id) {
    if (unit_id < 0)
        return;

    int lvd_fd = open(LVD_CTRL_PATH, O_RDWR);
    if (lvd_fd < 0)
        return;

    lvd_ioctl_detach_t req;
    memset(&req, 0, sizeof(req));
    req.device_id = unit_id;
    ioctl(lvd_fd, SCE_LVD_IOC_DETACH, &req);
    close(lvd_fd);
}

bool nmount_lvd_pfs_ex(const char *devname, const char *mount_point,
                       uint8_t nested_selector, bool force) {
    const char *sigverify = "0";
    const char *playgo = "0";
    const char *disc = "0";
    char mount_errmsg[256];
    memset(mount_errmsg, 0, sizeof(mount_errmsg));

    if (nested_selector != 0)
        disc = (nested_selector & 0x40u) ? "1" : "0";

    struct iovec iov_nested_pfs[] = {
        IOVEC_ENTRY("from"),      IOVEC_ENTRY(devname),
        IOVEC_ENTRY("fspath"),    IOVEC_ENTRY(mount_point),
        IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("pfs"),
        IOVEC_ENTRY("sigverify"), IOVEC_ENTRY(sigverify),
        IOVEC_ENTRY("mkeymode"),  IOVEC_ENTRY(PFS_MOUNT_MKEYMODE),
        IOVEC_ENTRY("budgetid"),  IOVEC_ENTRY(PFS_MOUNT_BUDGET_ID),
        IOVEC_ENTRY("playgo"),    IOVEC_ENTRY(playgo),
        IOVEC_ENTRY("disc"),      IOVEC_ENTRY(disc),
        IOVEC_ENTRY("ekpfs"),     IOVEC_ENTRY(PFS_ZERO_EKPFS_KEY_HEX),
        IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("automounted"), IOVEC_ENTRY(NULL),
        IOVEC_ENTRY("errmsg"),    {(void *)mount_errmsg, sizeof(mount_errmsg)},
        IOVEC_ENTRY("force"),     IOVEC_ENTRY(NULL),
    };

    unsigned int iovlen = (unsigned int)IOVEC_SIZE(iov_nested_pfs) - (force ? 1u : 2u);
    if (nmount(iov_nested_pfs, iovlen, MNT_RDONLY) == 0)
        return true;

    if (mount_errmsg[0] != '\0')
        notify("PFS nmount failed: %s", mount_errmsg);
    else
        notify("PFS nmount failed: %s", strerror(errno));
    return false;
}

bool nmount_lvd_pfs(const char *devname, const char *mount_point,
                    uint8_t nested_selector) {
    return nmount_lvd_pfs_ex(devname, mount_point, nested_selector, false);
}

static bool try_mount_lvd_pfs_once(const char *file_path, image_type_t fs_type,
                                   const char *mount_base, char *out_mount_point,
                                   int *unit_id_out, uint32_t sector_size) {
    uint8_t nested_selector = 0;
    if (fs_type == IMAGE_TYPE_PFSC)
        nested_selector = PFS_NESTED_OUTER_IMG_TYPE;
    else if (is_pfsc_mount_path(file_path))
        nested_selector = PFS_NESTED_INNER_IMG_TYPE;

    build_image_mount_point(file_path, fs_type, mount_base, out_mount_point);
    mkdir(mount_base, 0777);
    prepare_pfs_mount_dir(out_mount_point);
    if (mkdir(out_mount_point, 0777) != 0 && errno != EEXIST) {
        notify("mkdir failed for %s: %s", out_mount_point, strerror(errno));
        return false;
    }

    char devname[64];
    int unit_id = -1;
    if (!lvd_attach_pfs(file_path, nested_selector, sector_size, &unit_id, devname, sizeof(devname))) {
        rmdir(out_mount_point);
        return false;
    }

    if (!nmount_lvd_pfs(devname, out_mount_point, nested_selector)) {
        lvd_detach(unit_id);
        rmdir(out_mount_point);
        return false;
    }

    *unit_id_out = unit_id;
    return true;
}

bool mount_lvd_pfs_image(const char *file_path, image_type_t fs_type,
                         const char *mount_base, char *out_mount_point,
                         int *unit_id_out) {
    static const uint32_t sector_sizes[] = {
        LVD_SECTOR_SIZE_PFS,
        LVD_SECTOR_SIZE_PFS_LARGE,
    };

    for (size_t i = 0; i < sizeof(sector_sizes) / sizeof(sector_sizes[0]); i++) {
        if (try_mount_lvd_pfs_once(file_path, fs_type, mount_base, out_mount_point,
                                   unit_id_out, sector_sizes[i]))
            return true;
    }

    notify("LVD PFS mount failed for %s", file_path);
    return false;
}

bool mount_lvd_pfs_region(const char *container_path, uint64_t offset, uint64_t size,
                          const char *mount_base, char *out_mount_point, int *unit_id_out) {
    static const uint32_t sector_sizes[] = {
        LVD_SECTOR_SIZE_PFS,
        LVD_SECTOR_SIZE_PFS_LARGE,
    };

    if (!container_path || !mount_base || !out_mount_point || !unit_id_out || size == 0)
        return false;

    build_image_mount_point(container_path, IMAGE_TYPE_PFSC, mount_base, out_mount_point);
    mkdir(mount_base, 0777);
    prepare_pfs_mount_dir(out_mount_point);
    if (mkdir(out_mount_point, 0777) != 0 && errno != EEXIST) {
        notify("mkdir failed for %s: %s", out_mount_point, strerror(errno));
        return false;
    }

    for (size_t i = 0; i < sizeof(sector_sizes) / sizeof(sector_sizes[0]); i++) {
        char devname[64];
        int unit_id = -1;

        if (!lvd_attach_pfs_region_ex(container_path, offset, size, PFS_NESTED_INNER_IMG_TYPE,
                                      sector_sizes[i], &unit_id, devname, sizeof(devname)))
            continue;

        if (nmount_lvd_pfs_ex(devname, out_mount_point, PFS_NESTED_INNER_IMG_TYPE, true)) {
            *unit_id_out = unit_id;
            return true;
        }

        lvd_detach(unit_id);
    }

    notify("LVD PFS region mount failed for %s @%llu", container_path,
           (unsigned long long)offset);
    rmdir(out_mount_point);
    return false;
}

bool mount_lvd_pfs_image_at(const char *file_path, image_type_t fs_type,
                            const char *mount_point, int *unit_id_out) {
    static const uint32_t sector_sizes[] = {
        LVD_SECTOR_SIZE_PFS,
        LVD_SECTOR_SIZE_PFS_LARGE,
    };

    if (!file_path || !mount_point || !*mount_point || !unit_id_out)
        return false;

    struct statfs sfs;
    if (statfs(mount_point, &sfs) == 0 && strcmp(sfs.f_fstypename, "pfs") == 0)
        unmount(mount_point, MNT_FORCE);

    uint8_t nested_selector = 0;
    if (fs_type == IMAGE_TYPE_PFSC)
        nested_selector = PFS_NESTED_OUTER_IMG_TYPE;
    else if (is_pfsc_mount_path(file_path))
        nested_selector = PFS_NESTED_INNER_IMG_TYPE;

    for (size_t i = 0; i < sizeof(sector_sizes) / sizeof(sector_sizes[0]); i++) {
        char devname[64];
        int unit_id = -1;

        if (!lvd_attach_pfs(file_path, nested_selector, sector_sizes[i],
                            &unit_id, devname, sizeof(devname)))
            continue;

        if (nmount_lvd_pfs_ex(devname, mount_point, nested_selector, true)) {
            *unit_id_out = unit_id;
            return true;
        }

        lvd_detach(unit_id);
    }

    notify("LVD PFS mount failed for %s at %s", file_path, mount_point);
    return false;
}

bool relocate_lvd_pfs_mount(int unit_id, uint8_t nested_selector,
                            const char *old_mount, const char *new_mount) {
    if (unit_id < 0 || !old_mount || !new_mount)
        return false;

    char devname[64];
    snprintf(devname, sizeof(devname), "/dev/lvd%d", unit_id);

    if (mkdir(new_mount, 0755) != 0 && errno != EEXIST) {
        notify("mkdir failed for %s: %s", new_mount, strerror(errno));
        return false;
    }

    if (!nmount_lvd_pfs_ex(devname, new_mount, nested_selector, true)) {
        notify("PFS relocate failed: %s -> %s", old_mount, new_mount);
        return false;
    }

    struct statfs sfs;
    if (strcmp(old_mount, new_mount) != 0 &&
        statfs(old_mount, &sfs) == 0 && strcmp(sfs.f_fstypename, "pfs") == 0)
        unmount(old_mount, MNT_FORCE);

    return true;
}

void unmount_lvd_pfs(const char *mount_point, int unit_id) {
    if (!mount_point || !*mount_point)
        return;

    struct statfs sfs;
    if (statfs(mount_point, &sfs) == 0 && strcmp(sfs.f_fstypename, "pfs") == 0)
        unmount(mount_point, MNT_FORCE);

    lvd_detach(unit_id);

    if (rmdir(mount_point) != 0 && errno != ENOENT)
        notify("Failed to remove mount dir %s: %s", mount_point, strerror(errno));
}

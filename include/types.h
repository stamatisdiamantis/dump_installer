#ifndef TYPES_H
#define TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/param.h>
#include <sys/_iovec.h>
#include <sys/mount.h>
#include <sys/mdioctl.h>

#define IOVEC_ENTRY(x) { (void*)(x), (x) ? strlen(x) + 1 : 0 }
#define IOVEC_SIZE(x)  (sizeof(x) / sizeof(struct iovec))
#define MAX_PATH PATH_MAX

#define PFSC_IMAGE_MOUNT_BASE "/data/imgmnt/pfscmnt"
#define IMAGE_MOUNT_BASE      "/data/imgmnt"

#define LVD_CTRL_PATH           "/dev/lvdctl"
#define SCE_LVD_IOC_ATTACH_V0   0xC0286D00
#define SCE_LVD_IOC_DETACH      0xC0286D01

#define LVD_ATTACH_IO_VERSION_V0        0u
#define LVD_ATTACH_RAW_FLAGS_SINGLE_RO  0x9
#define LVD_ATTACH_IMAGE_TYPE_SINGLE    0
#define LVD_ATTACH_IMAGE_TYPE_PFS_SAVE_DATA 5
#define LVD_ENTRY_TYPE_FILE             1
#define LVD_ENTRY_FLAG_NO_BITMAP        0x1
#define LVD_SECTOR_SIZE_PFS             4096u
#define LVD_SECTOR_SIZE_PFS_LARGE       32768u
#define LVD_NODE_WAIT_US                100000
#define LVD_NODE_WAIT_RETRIES           100
#define LVD_SECONDARY_UNIT_SINGLE_IMAGE 0x10000u

#define PFS_NESTED_OUTER_IMG_TYPE 0x02u
#define PFS_NESTED_INNER_IMG_TYPE 0x82u

#define PFS_MOUNT_BUDGET_ID "game"
#define PFS_MOUNT_MKEYMODE  "AC"
#define PFS_ZERO_EKPFS_KEY_HEX \
    "0000000000000000000000000000000000000000000000000000000000000000"

#define LVD_SECTOR_SIZE_EXFAT   512u
#define LVD_IMAGE_TYPE_EXFAT    7
#define LVD_OPTION_EXFAT        0x16
#define LVD_OPTION_LEN_EXFAT    0x16

#define SCE_LVD_IOC_ATTACH      SCE_LVD_IOC_ATTACH_V0

typedef struct {
    uint8_t reserved;
    const char *budgetid;
} MountSaveDataOpt;

typedef struct {
    uint8_t dummy;
} UmountSaveDataOpt;

typedef struct {
    uint16_t source_type;
    uint8_t  entry_flags;
    uint8_t  reserved0;
    uint32_t reserved1;
    const char *path;
    uint64_t offset;
    uint64_t size;
    const char *bitmap_path;
    uint64_t bitmap_offset;
    uint64_t bitmap_size;
} lvd_kernel_layer_t;

typedef struct {
    uint32_t io_version;
    int32_t  device_id;
    uint32_t sector_size_0;
    uint32_t sector_size_1;
    uint16_t option_len;
    uint16_t image_type;
    uint32_t layer_count;
    uint64_t device_size;
    lvd_kernel_layer_t *layers_ptr;
} lvd_ioctl_attach_t;

typedef struct {
    uint16_t source_type;
    uint16_t flags;
    uint32_t reserved0;
    const char *path;
    uint64_t offset;
    uint64_t size;
    const char *bitmap_path;
    uint64_t bitmap_offset;
    uint64_t bitmap_size;
} lvd_ioctl_layer_v0_t;

typedef struct {
    uint32_t io_version;
    int32_t  device_id;
    uint32_t sector_size;
    uint32_t secondary_unit;
    uint16_t flags;
    uint16_t image_type;
    uint32_t layer_count;
    uint64_t device_size;
    lvd_ioctl_layer_v0_t *layers_ptr;
} lvd_ioctl_attach_v0_t;

typedef struct {
    uint32_t reserved0;
    int32_t  device_id;
    uint8_t  reserved[0x20];
} lvd_ioctl_detach_t;

typedef struct notify_request {
    char unused[45];
    char message[3075];
} notify_request_t;

typedef enum {
    IMAGE_TYPE_UNKNOWN = 0,
    IMAGE_TYPE_FOLDER,
    IMAGE_TYPE_PFS,
    IMAGE_TYPE_UFS,
    IMAGE_TYPE_EXFAT,
    IMAGE_TYPE_PFSC
} image_type_t;

#define MAX_DIR_IMAGES 64

typedef struct {
    char path[MAX_PATH];
    image_type_t type;
} dir_image_t;

#endif

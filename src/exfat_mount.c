#include "exfat_mount.h"
#include "mount_helpers.h"
#include "utils.h"
#include "log.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

bool mount_exfat_image(const char *file_path, char *out_mount_point) {
    return mount_exfat_image_ex(file_path, out_mount_point, false);
}

static bool mount_exfat_to_point(const char *file_path, const char *mount_point,
                                 bool skip_freshness_check, bool read_only) {
    struct stat st;
    if (!file_path || !mount_point || !*mount_point) {
        return false;
    }

    if (stat(file_path, &st) != 0) {
        notify("stat failed: %s", strerror(errno));
        return false;
    }

    if (!skip_freshness_check) {
        time_t now = time(NULL);
        if (difftime(now, st.st_mtime) < 12.0) {
            notify("Image too new (%.0fs) - skipping", difftime(now, st.st_mtime));
            return false;
        }
    }

    struct statfs sfs;
    if (statfs(mount_point, &sfs) == 0 && strcmp(sfs.f_fstypename, "exfatfs") == 0) {
        char probe[MAX_PATH];
        snprintf(probe, sizeof(probe), "%s/sce_sys/param.sfo", mount_point);
        if (access(probe, F_OK) == 0) {
            di_logf("EXFAT already mounted at %s", mount_point);
            return true;
        }
        di_logf("stale EXFAT mount at %s, remounting", mount_point);
        unmount(mount_point, MNT_FORCE);
    }

    if (mkdir(mount_point, 0777) != 0 && errno != EEXIST) {
        notify("mkdir failed: %s", strerror(errno));
        return false;
    }

    uint64_t offset = 0;
    int fd = open(file_path,O_RDONLY);
    if(fd>=0){
        uint8_t sector[512]; pread(fd,sector,512,0);
        if(!memcmp(sector+3,"EXFAT   ",8)) offset=0;
        close(fd);
    }

    // MD attach
    int md_fd=open("/dev/mdctl",O_RDWR);
    if(md_fd>=0){
        struct md_ioctl mdio={0};
        int md_ret;

        mdio.md_version=MDIOVERSION;
        mdio.md_type=MD_VNODE;
        mdio.md_file=(char*)file_path;
        mdio.md_mediasize=st.st_size-offset;
        mdio.md_sectorsize=512;
        mdio.md_options=MD_AUTOUNIT | (read_only ? MD_READONLY : 0);

        md_ret=ioctl(md_fd,(unsigned long)MDIOCATTACH,&mdio);
        if(md_ret!=0 && !read_only){
            mdio.md_options=MD_AUTOUNIT;
            md_ret=ioctl(md_fd,(unsigned long)MDIOCATTACH,&mdio);
        }

        if(md_ret==0){
            char devname[64];
            snprintf(devname,sizeof(devname),"/dev/md%u",mdio.md_unit);
            close(md_fd);
            struct iovec iov[]={
                IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
                IOVEC_ENTRY("from"),      IOVEC_ENTRY(devname),
                IOVEC_ENTRY("fspath"),    IOVEC_ENTRY(mount_point),
                IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
                IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
                IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
                IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
                IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
            };
            if(nmount(iov,IOVEC_SIZE(iov),read_only ? MNT_RDONLY : 0)==0){
                di_logf("exFAT mounted via MD at %s (%s)", mount_point,
                        read_only ? "ro" : "rw");
                return true;
            }
        } else {
            close(md_fd);
        }
    }

    // LVD fallback
    int lvd_fd=open("/dev/lvdctl",O_RDWR);
    if(lvd_fd>=0){
        lvd_kernel_layer_t layer={0};
        layer.source_type=1; 
		layer.entry_flags=0x1;
        layer.path=file_path; 
		layer.offset=0;
        layer.size=(uint64_t)st.st_size;

        lvd_ioctl_attach_t req={0};
        req.io_version=1;
		req.device_id=-1;
        req.sector_size_0=512u;
		req.sector_size_1=req.sector_size_0;
        req.image_type=7;
		req.layer_count=1;
        req.device_size=(uint64_t)st.st_size;
        req.layers_ptr=&layer;

        if(ioctl(lvd_fd,SCE_LVD_IOC_ATTACH,&req)==0 && req.device_id>=0){
            close(lvd_fd);
            char devname[64]; snprintf(devname,sizeof(devname),"/dev/lvd%d",req.device_id);
            struct iovec iov[]={
                IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
                IOVEC_ENTRY("from"),      IOVEC_ENTRY(devname),
                IOVEC_ENTRY("fspath"),    IOVEC_ENTRY(mount_point),
                IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
                IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
                IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
                IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
                IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
            };
            if(nmount(iov,IOVEC_SIZE(iov),read_only ? MNT_RDONLY : 0)==0){
                di_logf("exFAT mounted via LVD at %s", mount_point);
                return true;
            }
        } else close(lvd_fd);
    }

    di_logf("exFAT mount failed for %s -> %s errno=%d (%s)",
            file_path, mount_point, errno, strerror(errno));
    notify("exFAT mount failed completely");
    return false;
}

bool mount_exfat_image_at(const char *file_path, const char *mount_point,
                          bool skip_freshness_check) {
    return mount_exfat_to_point(file_path, mount_point, skip_freshness_check, true);
}

bool mount_exfat_image_at_rw(const char *file_path, const char *mount_point,
                             bool skip_freshness_check) {
    return mount_exfat_to_point(file_path, mount_point, skip_freshness_check, false);
}

bool mount_exfat_image_ex(const char *file_path, char *out_mount_point, bool skip_freshness_check) {
    const char *filename = strrchr(file_path, '/') ? strrchr(file_path, '/') + 1 : file_path;
    char mount_name[256];
    strncpy(mount_name, filename, sizeof(mount_name) - 1);
    mount_name[sizeof(mount_name) - 1] = '\0';
    char *dot = strrchr(mount_name, '.');
    if (dot)
        *dot = '\0';

    snprintf(out_mount_point, MAX_PATH, "/data/imgmnt/exfatmnt/%s", mount_name);
    return mount_exfat_to_point(file_path, out_mount_point, skip_freshness_check, true);
}

bool detach_exfat_md_mount(const char *mount_point) {
    struct statfs sfs;
    int unit = -1;

    if (!mount_point || !*mount_point)
        return false;

    if (statfs(mount_point, &sfs) != 0 || strcmp(sfs.f_fstypename, "exfatfs") != 0)
        return false;
    if (strcmp(sfs.f_mntonname, mount_point) != 0)
        return false;

    if (strncmp(sfs.f_mntfromname, "/dev/md", 7) == 0)
        unit = atoi(sfs.f_mntfromname + 7);

    di_logf("detaching exfat %s from %s", sfs.f_mntfromname, mount_point);
    sync();

    if (unmount(mount_point, MNT_FORCE) != 0) {
        di_logf("exfat unmount %s failed: %s", mount_point, strerror(errno));
        return false;
    }

    if (unit >= 0) {
        int mdctl = open("/dev/mdctl", O_RDWR);
        if (mdctl >= 0) {
            struct md_ioctl mdio = {0};
            mdio.md_version = MDIOVERSION;
            mdio.md_unit = (unsigned)unit;
            if (ioctl(mdctl, (unsigned long)MDIOCDETACH, &mdio) != 0)
                di_logf("MDIOCDETACH md%d failed: %s", unit, strerror(errno));
            close(mdctl);
        }
    }

    rmdir(mount_point);
    return true;
}

void unmount_exfat_mount_point(const char *mount_point) {
    struct statfs sfs;

    if (!mount_point || !*mount_point)
        return;

    if (statfs(mount_point, &sfs) == 0 && strcmp(sfs.f_fstypename, "exfatfs") == 0) {
        if (unmount(mount_point, MNT_FORCE) != 0)
            di_logf("exFAT unmount %s failed: %s", mount_point, strerror(errno));
    }

    rmdir(mount_point);
}

void unmount_exfat(const char* mount_point) {
    if (!mount_point || !*mount_point) return;

    unmount_exfat_mount_point(mount_point);

    for (int i = 0; i < 16; i++) {
        char devname[32];
        snprintf(devname, sizeof(devname), "/dev/lvd%d", i);
        if (access(devname, F_OK) == 0) {
            int lvdctl = open("/dev/lvdctl", O_RDWR);
            if (lvdctl >= 0) {
                lvd_ioctl_detach_t dreq = {0};
                dreq.device_id = i;
                ioctl(lvdctl, SCE_LVD_IOC_DETACH, &dreq);
                close(lvdctl);
            }
        }
    }

    for (int i = 0; i < 16; i++) {
        char devname[32];
        snprintf(devname, sizeof(devname), "/dev/md%d", i);
        if (access(devname, F_OK) != 0) continue;

        int mdctl = open("/dev/mdctl", O_RDWR);
        if (mdctl >= 0) {
            struct md_ioctl mdio = {0};
            mdio.md_version = MDIOVERSION;
            mdio.md_unit = i;
            ioctl(mdctl, (unsigned long)MDIOCDETACH, &mdio);
            close(mdctl);
        }
    }

    if (rmdir(mount_point) != 0 && errno != ENOENT) {
        notify("Failed to remove exFAT mount dir %s: %s", mount_point, strerror(errno));
    }
}

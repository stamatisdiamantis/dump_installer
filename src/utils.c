#include "utils.h"
#include "types.h"
#include "hash.h"
#include "log.h"

#include <stdarg.h>
#include <sys/stat.h>
#include <sys/aio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <sys/mount.h>

void notify(const char* fmt, ...) {
    notify_request_t req = {};
    va_list args;
    va_start(args, fmt);
    vsnprintf(req.message, sizeof(req.message) - 1, fmt, args);
    va_end(args);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

#define BUF_SIZE 0x800000
#define MAX_PARAM_JSON_SIZE (256 * 1024)

static int copy_param_json_rewrite(const char *src, const char *dst) {
    FILE *fs = fopen(src, "rb");
    if (!fs)
        return -1;

    if (fseek(fs, 0, SEEK_END) != 0) {
        fclose(fs);
        return -1;
    }

    long file_size = ftell(fs);
    if (file_size < 0 || (unsigned long)file_size > MAX_PARAM_JSON_SIZE) {
        fclose(fs);
        return -1;
    }

    rewind(fs);

    size_t len = (size_t)file_size;
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        fclose(fs);
        return -1;
    }

    if (len > 0 && fread(buf, 1, len, fs) != len) {
        free(buf);
        fclose(fs);
        return -1;
    }
    fclose(fs);
    buf[len] = '\0';

    char *hit = strstr(buf, "upgradable");
    if (hit) {
        size_t tail = len - (size_t)(hit - buf) - 10u;
        memcpy(hit, "standard", 8u);
        memmove(hit + 8u, hit + 10u, tail + 1u);
        len -= 2u;
    }

    FILE *fd = fopen(dst, "wb");
    if (!fd) {
        free(buf);
        return -1;
    }

    int ret = 0;
    if (len > 0 && fwrite(buf, 1, len, fd) != len)
        ret = -1;
    if (fclose(fd) != 0)
        ret = -1;

    free(buf);
    return ret;
}

int copy_file(const char* src, const char* dst) {
    if (strstr(src, "/sce_sys/param.json"))
        return copy_param_json_rewrite(src, dst);

    struct stat st;
    if (stat(src, &st) != 0 || !S_ISREG(st.st_mode)) return -1;

    int fd_in = open(src, O_RDONLY);
    if (fd_in < 0) return -1;
    int fd_out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode | 0600);
    if (fd_out < 0) { close(fd_in); return -1; }

    void* buf = malloc(BUF_SIZE);
    if (!buf) { close(fd_in); close(fd_out); return -1; }

    struct aiocb aior = {0};
    struct aiocb aiow = {0};
    aior.aio_fildes = fd_in;
    aior.aio_buf = buf;
    aiow.aio_fildes = fd_out;

    size_t copied = 0;
    const struct aiocb* aiolist[1];

    while (copied < (size_t)st.st_size) {
        size_t chunk = BUF_SIZE;
        if (copied + chunk > (size_t)st.st_size) chunk = st.st_size - copied;

        aior.aio_nbytes = chunk;
        aior.aio_offset = copied;
        if (aio_read(&aior) < 0) break;

        aiolist[0] = &aior;
        if (aio_suspend(aiolist, 1, NULL) < 0) break;

        ssize_t n = aio_return(&aior);
        if (n <= 0) break;

        aiow.aio_buf = buf;
        aiow.aio_nbytes = n;
        aiow.aio_offset = copied;
        if (aio_write(&aiow) < 0) break;

        aiolist[0] = &aiow;
        if (aio_suspend(aiolist, 1, NULL) < 0) break;
        if (aio_return(&aiow) < 0) break;

        copied += n;
    }

    free(buf);
    close(fd_in);
    close(fd_out);

    return copied == (size_t)st.st_size ? 0 : -1;
}

int copy_dir(const char* src, const char* dst) {
    if (mkdir(dst, 0755) && errno != EEXIST) return -1;

    DIR* d = opendir(src);
    if (!d) {
        notify("copy_dir: Cannot open source dir %s: %s", src, strerror(errno));
        return -1;
    }

    struct dirent* e;
    char src_path[MAX_PATH];
    char dst_path[MAX_PATH];

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        snprintf(src_path, sizeof(src_path), "%s/%s", src, e->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst, e->d_name);

        struct stat st;
        if (lstat(src_path, &st) != 0) {
            if (errno != ENOENT)
                notify("copy_dir: lstat failed on %s: %s", src_path, strerror(errno));
            continue;
        }

        if (S_ISLNK(st.st_mode))
            continue;

        if (S_ISDIR(st.st_mode)) {
            copy_dir(src_path, dst_path);
        } else if (S_ISREG(st.st_mode)) {
            copy_file(src_path, dst_path);
        }
    }

    closedir(d);
    return 0;
}

int remove_dir(const char* path) {
    struct statfs sfs;

    if (path && statfs(path, &sfs) == 0 && strcmp(sfs.f_mntonname, path) == 0) {
        di_logf("refusing remove_dir on mount point: %s", path);
        return -1;
    }

    DIR* d = opendir(path); if (!d) return -1;
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char fullpath[PATH_MAX];
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
        struct stat st;
        if (stat(fullpath, &st) == -1) continue;
        if (S_ISDIR(st.st_mode)) remove_dir(fullpath);
        else unlink(fullpath);
    }
    closedir(d);
    return rmdir(path);
}

static const uint8_t *find_memmem(const uint8_t *hay, size_t hay_len,
                                  const uint8_t *needle, size_t needle_len) {
    size_t i;

    if (!hay || !needle || needle_len == 0 || hay_len < needle_len)
        return NULL;

    for (i = 0; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0)
            return hay + i;
    }

    return NULL;
}

static void pfsc_cache_path(const char *container_path, uint64_t inner_size,
                            char *out, size_t out_sz) {
    snprintf(out, out_sz, "/data/imgmnt/pfscache/%08x_%08x.off",
             fnv1a32(container_path), (unsigned)(inner_size & 0xffffffffu));
}

bool read_pfsc_inner_offset_cache(const char *container_path, uint64_t inner_size,
                                  uint64_t *offset_out) {
    char path[MAX_PATH];
    FILE *f;
    unsigned long long off = 0;
    unsigned long long sz = 0;

    if (!container_path || !offset_out || inner_size == 0)
        return false;

    pfsc_cache_path(container_path, inner_size, path, sizeof(path));
    f = fopen(path, "r");
    if (!f)
        return false;

    if (fscanf(f, "%llu %llu", &off, &sz) != 2 || sz != inner_size) {
        fclose(f);
        return false;
    }

    fclose(f);
    *offset_out = off;
    di_logf("cached inner offset %llu for %s", off, container_path);
    return true;
}

void write_pfsc_inner_offset_cache(const char *container_path, uint64_t inner_size,
                                   uint64_t offset) {
    char path[MAX_PATH];
    FILE *f;

    if (!container_path || inner_size == 0)
        return;

    mkdir("/data/imgmnt/pfscache", 0755);
    pfsc_cache_path(container_path, inner_size, path, sizeof(path));
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%llu %llu\n", (unsigned long long)offset, (unsigned long long)inner_size);
    fclose(f);
    di_logf("saved inner offset cache: %s", path);
}

bool find_subfile_offset_in_container(const char *container_path, const char *inner_path,
                                      uint64_t inner_size, uint64_t *offset_out) {
    size_t sig_len;
    size_t tail_len;
    uint8_t sig[4096];
    uint8_t tail[4096];
    int inner_fd = -1;
    int container_fd = -1;
    struct stat container_st;
    bool found = false;

    if (!container_path || !inner_path || !offset_out || inner_size == 0)
        return false;

    if (read_pfsc_inner_offset_cache(container_path, inner_size, offset_out))
        return true;

    sig_len = inner_size < sizeof(sig) ? (size_t)inner_size : sizeof(sig);
    inner_fd = open(inner_path, O_RDONLY);
    if (inner_fd < 0 || read(inner_fd, sig, sig_len) != (ssize_t)sig_len)
        goto done;

    tail_len = 0;
    if (inner_size > sig_len) {
        tail_len = inner_size < sizeof(tail) ? (size_t)inner_size : sizeof(tail);
        if (pread(inner_fd, tail, tail_len, (off_t)(inner_size - tail_len)) != (ssize_t)tail_len)
            goto done;
    }

    container_fd = open(container_path, O_RDONLY);
    if (container_fd < 0 || fstat(container_fd, &container_st) != 0)
        goto done;

    const size_t window = 64u * 1024u * 1024u;
    const size_t overlap = sig_len > 0 ? sig_len - 1u : 0u;
    uint64_t base = 0;

    while (base < (uint64_t)container_st.st_size) {
        size_t map_len = window;

        if (base + map_len > (uint64_t)container_st.st_size)
            map_len = (size_t)((uint64_t)container_st.st_size - base);
        if (map_len < sig_len)
            break;

        void *map = mmap(NULL, map_len, PROT_READ, MAP_SHARED, container_fd, (off_t)base);
        if (map == MAP_FAILED)
            break;

        const uint8_t *start = (const uint8_t *)map;
        const uint8_t *cursor = start;
        size_t remaining = map_len - sig_len;

        while (remaining > 0) {
            const uint8_t *hit = find_memmem(cursor, remaining, sig, sig_len);
            if (!hit)
                break;

            uint64_t candidate = base + (uint64_t)(hit - start);
            if (candidate + inner_size <= (uint64_t)container_st.st_size) {
                bool tail_ok = true;
                if (tail_len > 0) {
                    uint8_t probe[4096];
                    if (pread(container_fd, probe, tail_len,
                              (off_t)(candidate + inner_size - tail_len)) != (ssize_t)tail_len ||
                        memcmp(probe, tail, tail_len) != 0) {
                        tail_ok = false;
                    }
                }
                if (tail_ok) {
                    *offset_out = candidate;
                    found = true;
                    write_pfsc_inner_offset_cache(container_path, inner_size, candidate);
                    munmap(map, map_len);
                    di_logf("found %s in %s at offset %llu",
                            inner_path, container_path, (unsigned long long)candidate);
                    goto done;
                }
            }

            size_t advance = (size_t)(hit - cursor) + 1u;
            cursor += advance;
            remaining -= advance;
        }

        munmap(map, map_len);
        if (base + map_len >= (uint64_t)container_st.st_size)
            break;
        base += map_len - overlap;
    }

done:
    if (inner_fd >= 0)
        close(inner_fd);
    if (container_fd >= 0)
        close(container_fd);
    return found;
}

int is_appmeta_file(const char* name) {
    if (!strcasecmp(name, "param.json") || !strcasecmp(name, "param.sfo")) return 1;
    const char* ext = strrchr(name, '.');
    if (!ext) return 0;
    return !strcasecmp(ext, ".png") || !strcasecmp(ext, ".dds") || !strcasecmp(ext, ".at9");
}
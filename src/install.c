#include "install.h"
#include "utils.h"
#include "types.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <ctype.h>
#include <sqlite3.h>

int copy_sce_sys_to_appmeta(const char* src, const char* title_id) {
    char dst[MAX_PATH];
    snprintf(dst, sizeof(dst), "/user/appmeta/%s", title_id);
    mkdir("/user/appmeta", 0777);
    mkdir(dst, 0755);

    DIR* d = opendir(src);
    if (!d) return -1;

    struct dirent* e;
    char ss[MAX_PATH], dd[MAX_PATH];
    struct stat st;

    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!is_appmeta_file(e->d_name)) continue;

        snprintf(ss, sizeof(ss), "%s/%s", src, e->d_name);
        snprintf(dd, sizeof(dd), "%s/%s", dst, e->d_name);

        if (stat(ss, &st) != 0 || !S_ISREG(st.st_mode)) continue;

        copy_file(ss, dd);
    }

    closedir(d);
    return 0;
}

static int copy_trophy_file(const char* src, const char* dst) {
    if (access(src, F_OK) != 0)
        return 0;
    if (copy_file(src, dst) == 0)
        return 0;
    return -1;
}

int update_trophy(const char* title_id, const char* src_sce_sys) {
    char dst_base[MAX_PATH];
    char src[MAX_PATH];
    char dst[MAX_PATH];
    int failures = 0;

    snprintf(dst_base, sizeof(dst_base), "/system_data/priv/appmeta/%s", title_id);

    mkdir("/system_data/priv/appmeta", 0755);
    if (mkdir(dst_base, 0755) && errno != EEXIST)
        failures++;

    char trophy2_dir[MAX_PATH];
    char uds_dir[MAX_PATH];
    snprintf(trophy2_dir, sizeof(trophy2_dir), "%s/trophy2", dst_base);
    snprintf(uds_dir, sizeof(uds_dir), "%s/uds", dst_base);

    mkdir(trophy2_dir, 0755);
    mkdir(uds_dir, 0755);

    snprintf(src, sizeof(src), "%s/trophy2/npbind.dat", src_sce_sys);
    snprintf(dst, sizeof(dst), "%s/trophy2/npbind.dat", dst_base);
    if (copy_trophy_file(src, dst) != 0)
        failures++;

    snprintf(src, sizeof(src), "%s/uds/npbind.dat", src_sce_sys);
    snprintf(dst, sizeof(dst), "%s/uds/npbind.dat", dst_base);
    if (copy_trophy_file(src, dst) != 0)
        failures++;

    snprintf(src, sizeof(src), "%s/param.json", src_sce_sys);
    snprintf(dst, sizeof(dst), "%s/param.json", dst_base);
    if (copy_trophy_file(src, dst) != 0)
        failures++;

    return failures > 0 ? -1 : 0;
}

int update_snd0info(const char* title_id) {
    sqlite3* db = NULL;
    sqlite3_stmt* stmt = NULL;
    int ret = -1;
    char db_path[] = "/system_data/priv/mms/app.db";
    const char* sql = "UPDATE tbl_contentinfo SET snd0info = '/user/appmeta/' || ?1 || '/snd0.at9' WHERE titleId = ?1;";

    if (sqlite3_open(db_path, &db) != SQLITE_OK) goto out;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) goto out;
    sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
    sqlite3_step(stmt);  // ignore result, we don't care if row existed
    ret = sqlite3_changes(db);

out:
    if (stmt) sqlite3_finalize(stmt);
    if (db) sqlite3_close(db);
    return ret;
}

int extract_json_string(const char* json, const char* key, char* out, size_t out_size) {
    char search[64];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char* p = strstr(json, search);
    if (!p) return -1;

    p = strchr(p + strlen(search), ':');
    if (!p) return -1;

    while (*++p && isspace(*p));
    if (*p != '"') return -1;
    p++;

    size_t i = 0;
    while (i < out_size - 1 && p[i] && p[i] != '"') {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
    return 0;
}

typedef struct {
    uint16_t key_offset;
    uint16_t type;
    uint32_t size;
    uint32_t max_size;
    uint32_t data_offset;
} sfo_entry_t;

int read_title_id_from_sfo(const char* path, char* title_id, size_t size) {
    FILE* f = fopen(path, "rb");
    if (!f) return -1;

    uint32_t magic, version, key_off, data_off, count;
    if (fread(&magic, 4, 1, f) != 1 || fread(&version, 4, 1, f) != 1 ||
        fread(&key_off, 4, 1, f) != 1 || fread(&data_off, 4, 1, f) != 1 ||
        fread(&count, 4, 1, f) != 1) {
        fclose(f);
        return -1;
    }

    if (magic != 0x46535000) {
        fclose(f);
        return -1;
    }

    for (uint32_t i = 0; i < count; i++) {
        sfo_entry_t entry;
        if (fseek(f, 0x14 + i * sizeof(sfo_entry_t), SEEK_SET) != 0) continue;
        if (fread(&entry, sizeof(sfo_entry_t), 1, f) != 1) continue;

        char key[128] = {0};
        if (fseek(f, key_off + entry.key_offset, SEEK_SET) != 0) continue;
        if (fread(key, 1, sizeof(key) - 1, f) <= 0) continue;

        for (int k = 0; k < 128; k++) {
            if (!isprint(key[k])) {
                key[k] = '\0';
                break;
            }
        }

        if (strncmp(key, "TITLE_ID", 8) == 0) {
            if (fseek(f, data_off + entry.data_offset, SEEK_SET) != 0) continue;
            size_t rlen = (entry.size < size - 1) ? entry.size : size - 1;
            if (fread(title_id, 1, rlen, f) <= 0) continue;
            title_id[rlen] = '\0';
            fclose(f);
            return 0;
        }
    }

    fclose(f);
    return -1;
}

int get_title_id(const char* base_path, char* title_id, size_t size) {
    char path[MAX_PATH];  // ← declare path here

    snprintf(path, sizeof(path), "%s/sce_sys/param.json", base_path);
    FILE* f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (len > 0 && len < 1024 * 1024) {
            char* buf = (char*)malloc(len + 1);
            if (buf) {
                fread(buf, 1, len, f);
                buf[len] = '\0';
                if (extract_json_string(buf, "titleId", title_id, size) == 0 ||
                    extract_json_string(buf, "title_id", title_id, size) == 0) {
                    free(buf);
                    fclose(f);
                    return 0;
                }
                free(buf);
            }
        }
        fclose(f);
    }

    snprintf(path, sizeof(path), "%s/sce_sys/param.sfo", base_path);
    return read_title_id_from_sfo(path, title_id, size);
}
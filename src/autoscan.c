#include "autoscan.h"
#include "image.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static bool entry_is_game_file(const char *name) {
    return name && detect_image_type_for_path(NULL, name) != IMAGE_TYPE_UNKNOWN;
}

static bool is_ignored_subdir(const char *name) {
    if (!name || name[0] == '.')
        return true;
    if (!strcmp(name, "dump_installer"))
        return true;
    if (!strncmp(name, ".di_stage_", 10))
        return true;
    return false;
}

static bool path_is_dir(const char *path) {
    struct stat st;

    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void append_location(char locations[][MAX_PATH], int *count, int max,
                            const char *path) {
    int i;

    if (!path || !path[0] || !locations || !count || *count >= max)
        return;

    for (i = 0; i < *count; i++) {
        if (!strcmp(locations[i], path))
            return;
    }

    if (!path_is_dir(path))
        return;

    snprintf(locations[*count], MAX_PATH, "%s", path);
    (*count)++;
}

static void append_mnt_roots(char locations[][MAX_PATH], int *count, int max) {
    DIR *d = opendir("/mnt");
    struct dirent *e;

    if (!d)
        return;

    while ((e = readdir(d)) && *count < max) {
        char root[MAX_PATH];
        char homebrew[MAX_PATH];
        char etahen[MAX_PATH];

        if (e->d_name[0] == '.')
            continue;

        if (strncmp(e->d_name, "usb", 3) != 0 && strncmp(e->d_name, "ext", 3) != 0)
            continue;

        snprintf(root, sizeof(root), "/mnt/%s", e->d_name);
        snprintf(homebrew, sizeof(homebrew), "/mnt/%s/homebrew", e->d_name);
        snprintf(etahen, sizeof(etahen), "/mnt/%s/etaHEN/games", e->d_name);

        append_location(locations, count, max, homebrew);
        append_location(locations, count, max, etahen);
        append_location(locations, count, max, root);
    }

    closedir(d);
}

int autoscan_build_locations(char locations[][MAX_PATH], int max_locations) {
    static const char *fixed[] = {
        "/data/homebrew",
        "/data/etaHEN/games",
        "/mnt/ext0/homebrew",
        "/mnt/ext0/etaHEN/games",
        "/mnt/ext1/homebrew",
        "/mnt/ext1/etaHEN/games",
        "/mnt/ext0",
        "/mnt/ext1",
        "/mnt/shadowmnt/pfsc",
        "/mnt/shadowmnt",
        "/mnt/shadowmount/pfsc",
        "/mnt/shadowmount",
        NULL
    };
    int count = 0;
    char usb_homebrew[MAX_PATH];
    char usb_etahen[MAX_PATH];
    char usb_root[MAX_PATH];

    if (!locations || max_locations <= 0)
        return 0;

    for (int i = 0; fixed[i] && count < max_locations; i++)
        append_location(locations, &count, max_locations, fixed[i]);

    for (int i = 0; i <= 7 && count < max_locations; i++) {
        snprintf(usb_homebrew, sizeof(usb_homebrew), "/mnt/usb%d/homebrew", i);
        snprintf(usb_etahen, sizeof(usb_etahen), "/mnt/usb%d/etaHEN/games", i);
        snprintf(usb_root, sizeof(usb_root), "/mnt/usb%d", i);
        append_location(locations, &count, max_locations, usb_homebrew);
        append_location(locations, &count, max_locations, usb_etahen);
        append_location(locations, &count, max_locations, usb_root);
    }

    append_mnt_roots(locations, &count, max_locations);
    return count;
}

int autoscan_count_installables(const char *dir) {
    char path[MAX_PATH];
    int count = 0;
    DIR *d;

    if (!dir || !dir[0])
        return 0;

    d = opendir(dir);
    if (!d)
        return 0;

    snprintf(path, sizeof(path), "%s/sce_sys", dir);
    if (path_is_dir(path))
        count++;

    while (true) {
        struct dirent *e = readdir(d);

        if (!e)
            break;
        if (e->d_name[0] == '.')
            continue;

        if (entry_is_game_file(e->d_name)) {
            count++;
            continue;
        }

        if (is_ignored_subdir(e->d_name))
            continue;

        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (!path_is_dir(path))
            continue;

        {
            DIR *sub = opendir(path);
            bool has_game = false;
            bool has_sce_sys = false;

            if (!sub)
                continue;

            while (true) {
                struct dirent *se = readdir(sub);

                if (!se)
                    break;
                if (se->d_name[0] == '.')
                    continue;
                if (entry_is_game_file(se->d_name))
                    has_game = true;
                if (!strcmp(se->d_name, "sce_sys"))
                    has_sce_sys = true;
            }

            closedir(sub);
            if (has_game || has_sce_sys)
                count++;
        }
    }

    closedir(d);
    return count;
}

bool autoscan_dir_has_games(const char *dir) {
    return autoscan_count_installables(dir) > 0;
}

bool autoscan_any_games_found(void) {
    char locations[48][MAX_PATH];
    int count = autoscan_build_locations(locations, 48);

    for (int i = 0; i < count; i++) {
        if (autoscan_dir_has_games(locations[i]))
            return true;
    }

    return false;
}

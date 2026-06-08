#ifndef AUTOSCAN_H
#define AUTOSCAN_H

#include "types.h"

#include <stdbool.h>

int autoscan_build_locations(char locations[][MAX_PATH], int max_locations);
int autoscan_count_installables(const char *dir);
bool autoscan_dir_has_games(const char *dir);
bool autoscan_any_games_found(void);

#endif

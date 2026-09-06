#pragma once

#include <stddef.h>


/* Locate BG3's per-profile input config without assuming a user name. */
int LNCT_FindInputConfigPath(char* path, size_t path_size);

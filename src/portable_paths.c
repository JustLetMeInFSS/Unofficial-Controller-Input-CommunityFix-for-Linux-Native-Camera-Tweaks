#include "portable_paths.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define LNCT_INPUT_CONFIG_ENV "BG3_INPUT_CONFIG_PATH"
#define LNCT_PROFILE_SUFFIX "Larian Studios/Baldur's Gate 3/PlayerProfiles"
#define LNCT_INPUT_CONFIG_FILENAME "inputconfig_p1.json"

static int IsAbsolute(const char* path)
{
	return path && path[0] == '/';
}

static int IsReadableFile(const char* path)
{
	return path && access(path, R_OK) == 0;
}

static int CopyReadablePath(const char* source, char* output, size_t output_size)
{
	if (!IsReadableFile(source))
		return 0;
	int length = snprintf(output, output_size, "%s", source);
	return length > 0 && (size_t)length < output_size;
}

static int FindBelowDataHome(const char* data_home, char* output, size_t output_size)
{
	if (!IsAbsolute(data_home))
		return 0;

	char profiles[1024];
	int length = snprintf(profiles, sizeof(profiles), "%s/%s", data_home, LNCT_PROFILE_SUFFIX);
	if (length <= 0 || (size_t)length >= sizeof(profiles))
		return 0;

	char candidate[1200];
	length = snprintf(candidate, sizeof(candidate), "%s/Public/%s", profiles, LNCT_INPUT_CONFIG_FILENAME);
	if (length > 0 && (size_t)length < sizeof(candidate)
		&& CopyReadablePath(candidate, output, output_size))
	{
		return 1;
	}

	DIR* directory = opendir(profiles);
	if (!directory)
		return 0;

	int found = 0;
	struct dirent* entry;
	while (!found && (entry = readdir(directory)) != NULL)
	{
		if (entry->d_name[0] == '.')
			continue;
		length = snprintf(candidate, sizeof(candidate), "%s/%s/%s",
			profiles, entry->d_name, LNCT_INPUT_CONFIG_FILENAME);
		if (length > 0 && (size_t)length < sizeof(candidate))
			found = CopyReadablePath(candidate, output, output_size);
	}
	closedir(directory);
	return found;
}

int LNCT_FindInputConfigPath(char* path, size_t path_size)
{
	const char* override = getenv(LNCT_INPUT_CONFIG_ENV);
	if (IsAbsolute(override) && CopyReadablePath(override, path, path_size))
		return 1;

	const char* data_home = getenv("XDG_DATA_HOME");
	if (FindBelowDataHome(data_home, path, path_size))
		return 1;

	const char* home = getenv("HOME");
	if (!IsAbsolute(home))
		return 0;

	char default_data_home[1024];
	int length = snprintf(default_data_home, sizeof(default_data_home), "%s/.local/share", home);
	if (length <= 0 || (size_t)length >= sizeof(default_data_home))
		return 0;
	return FindBelowDataHome(default_data_home, path, path_size);
}

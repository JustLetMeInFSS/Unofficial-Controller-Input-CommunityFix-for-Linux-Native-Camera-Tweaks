#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "portable_paths.h"


static void MakeDirectory(const char* path)
{
	assert(mkdir(path, 0755) == 0);
}

static void MakeFile(const char* path)
{
	FILE* file = fopen(path, "w");
	assert(file);
	fputs("{}", file);
	fclose(file);
}

int main(void)
{
	char root_template[] = "/tmp/lnct-paths-XXXXXX";
	char* root = mkdtemp(root_template);
	assert(root);

	char xdg[1024];
	char path[1200];
	snprintf(xdg, sizeof(xdg), "%s/data", root);
	MakeDirectory(xdg);
	snprintf(path, sizeof(path), "%s/Larian Studios", xdg);
	MakeDirectory(path);
	strcat(path, "/Baldur's Gate 3");
	MakeDirectory(path);
	strcat(path, "/PlayerProfiles");
	MakeDirectory(path);
	strcat(path, "/CustomProfile");
	MakeDirectory(path);
	strcat(path, "/inputconfig_p1.json");
	MakeFile(path);

	assert(setenv("XDG_DATA_HOME", xdg, 1) == 0);
	assert(unsetenv("BG3_INPUT_CONFIG_PATH") == 0);
	char discovered[1200];
	assert(LNCT_FindInputConfigPath(discovered, sizeof(discovered)));
	assert(strcmp(discovered, path) == 0);

	char override[1200];
	snprintf(override, sizeof(override), "%s/override.json", root);
	MakeFile(override);
	assert(setenv("BG3_INPUT_CONFIG_PATH", override, 1) == 0);
	assert(LNCT_FindInputConfigPath(discovered, sizeof(discovered)));
	assert(strcmp(discovered, override) == 0);
	return 0;
}

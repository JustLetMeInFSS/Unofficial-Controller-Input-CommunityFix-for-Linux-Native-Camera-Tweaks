#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "file_stamp.h"


static void WriteFile(const char* path, const char* content)
{
	FILE* file = fopen(path, "w");
	assert(file);
	fputs(content, file);
	assert(fclose(file) == 0);
}

int main(void)
{
	char root_template[] = "/tmp/lnct-stamp-XXXXXX";
	char* root = mkdtemp(root_template);
	assert(root);

	char path[512];
	char replacement[512];
	snprintf(path, sizeof(path), "%s/inputconfig_p1.json", root);
	snprintf(replacement, sizeof(replacement), "%s/replacement.json", root);
	WriteFile(path, "{}");

	LNCT_FileStamp first;
	LNCT_FileStamp unchanged;
	assert(LNCT_ReadFileStamp(path, &first));
	assert(LNCT_ReadFileStamp(path, &unchanged));
	assert(LNCT_FileStampEqual(&first, &unchanged));

	WriteFile(replacement, "{}");
	assert(rename(replacement, path) == 0);
	LNCT_FileStamp replaced;
	assert(LNCT_ReadFileStamp(path, &replaced));
	assert(!LNCT_FileStampEqual(&first, &replaced));

	assert(unlink(path) == 0);
	LNCT_FileStamp missing;
	assert(!LNCT_ReadFileStamp(path, &missing));
	assert(!missing.valid);
	return 0;
}

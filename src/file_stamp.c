#include "file_stamp.h"

#include <fcntl.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__linux__) && defined(SYS_newfstatat)
static int LNCT_StatCompat(const char* path, struct stat* status)
{
	return (int)syscall(SYS_newfstatat, AT_FDCWD, path, status, 0);
}
#else
#define LNCT_StatCompat stat
#endif


int LNCT_ReadFileStamp(const char* path, LNCT_FileStamp* stamp)
{
	struct stat status;
	memset(stamp, 0, sizeof(*stamp));
	if (!path || !path[0] || LNCT_StatCompat(path, &status) != 0)
		return 0;

	stamp->device = status.st_dev;
	stamp->inode = status.st_ino;
	stamp->size = status.st_size;
	stamp->modified = status.st_mtim;
	stamp->valid = 1;
	return 1;
}

int LNCT_FileStampEqual(const LNCT_FileStamp* left, const LNCT_FileStamp* right)
{
	return left->valid && right->valid
		&& left->device == right->device
		&& left->inode == right->inode
		&& left->size == right->size
		&& left->modified.tv_sec == right->modified.tv_sec
		&& left->modified.tv_nsec == right->modified.tv_nsec;
}

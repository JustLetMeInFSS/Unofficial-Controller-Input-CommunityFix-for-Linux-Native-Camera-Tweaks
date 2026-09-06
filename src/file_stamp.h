#pragma once

#include <sys/stat.h>


typedef struct
{
	dev_t device;
	ino_t inode;
	off_t size;
	struct timespec modified;
	int valid;
} LNCT_FileStamp;

int LNCT_ReadFileStamp(const char* path, LNCT_FileStamp* stamp);
int LNCT_FileStampEqual(const LNCT_FileStamp* left, const LNCT_FileStamp* right);

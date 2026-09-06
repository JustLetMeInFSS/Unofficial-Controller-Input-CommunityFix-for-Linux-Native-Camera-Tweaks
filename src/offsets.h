#pragma once

#include <stdint.h>


struct Addresses
{
	uint64_t CalculateCameraAngle_CallSite;
	uint64_t roll_movss;
};
struct Addresses* GetAddresses(void);

struct Sigs
{
	const char* CalculateCameraAngle_Callsite;
	const char* roll_movss;
};
struct Sigs* GetSigs(void);

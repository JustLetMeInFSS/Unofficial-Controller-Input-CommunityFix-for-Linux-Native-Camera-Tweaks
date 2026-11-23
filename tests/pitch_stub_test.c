/*
 * Executes the pitch-store stub for real: builds a tiny function containing
 * BG3's `movss [rbp+0x164], xmmK`, patches it exactly like PatchUpdateCamera
 * does, and checks the stored pitch, the register BG3 keeps using after the
 * store (written to cam+0x100), and that rax/rcx survive.
 */
#include "../src/main.c"

#include <assert.h>

typedef uint32_t (*Harness)(void* camera, uint32_t pitch_bits, uint32_t marker);

static Harness BuildHarness(int xmm, uint8_t** out_code, uint8_t** out_site)
{
	long page = sysconf(_SC_PAGESIZE);
	uint8_t* code = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	assert(code != MAP_FAILED);
	size_t o = 0;
	code[o++] = 0x55;                                     /* push rbp */
	code[o++] = 0x48; code[o++] = 0x89; code[o++] = 0xFD; /* mov rbp,rdi */
	code[o++] = 0x66; code[o++] = 0x0F; code[o++] = 0x6E;
	code[o++] = (uint8_t)(0xC6 | (xmm << 3));             /* movd xmmK,esi */
	code[o++] = 0x89; code[o++] = 0xD0;                   /* mov eax,edx */
	code[o++] = 0x89; code[o++] = 0xD1;                   /* mov ecx,edx */
	uint8_t* site = code + o;
	const uint8_t movss[8] = { 0xF3, 0x0F, 0x11, (uint8_t)(0x85 | (xmm << 3)), 0x64, 0x01, 0x00, 0x00 };
	memcpy(code + o, movss, 8); o += 8;
	code[o++] = 0x01; code[o++] = 0xC8;                   /* add eax,ecx */
	code[o++] = 0x66; code[o++] = 0x0F; code[o++] = 0x7E;
	code[o++] = (uint8_t)(0xC2 | (xmm << 3));             /* movd edx,xmmK */
	code[o++] = 0x89; code[o++] = 0x95;
	code[o++] = 0x00; code[o++] = 0x01; code[o++] = 0x00; code[o++] = 0x00; /* mov [rbp+0x100],edx */
	code[o++] = 0x5D;                                     /* pop rbp */
	code[o++] = 0xC3;                                     /* ret */
	*out_code = code;
	*out_site = site;
	return (Harness)(void*)code;
}

static uint32_t Bits(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }

int main(void)
{
	for (int xmm = 0; xmm < 2; xmm++)
	{
		uint8_t* code;
		uint8_t* site;
		Harness f = BuildHarness(xmm, &code, &site);
		assert(IsExpectedPitchStore(site));

		/* Original instruction stores unconditionally. */
		uint8_t cam[0x200];
		memset(cam, 0, sizeof(cam));
		assert(f(cam, Bits(12.5f), 7) == 14);
		assert(*(float*)(cam + 0x164) == 12.5f);

		/* Patch it like PatchUpdateCamera does. */
		long page = sysconf(_SC_PAGESIZE);
		uint8_t* stub = AllocNear(site, (size_t)page);
		assert(stub);
		volatile uint8_t* enable = stub + PITCH_STUB_ENABLE_OFFSET;
		*enable = 0;
		BuildPitchStoreStub(stub, site, enable);
		uint8_t jump[8];
		BuildPitchStoreJump(jump, site, stub);
		memcpy(site, jump, 8);

		float tagged_old = TagPitch(30.f, 1); /* player 2 tag */
		*(float*)(cam + 0x164) = tagged_old;

		/* Disabled: no store, even in combat. */
		*(uint32_t*)(cam + CAMERA_OBJECT_MODE_FLAGS_OFFSET) = CAMERA_MODE_COMBAT;
		assert(f(cam, Bits(45.f), 1000) == 2000);
		assert(*(float*)(cam + 0x164) == tagged_old);

		/* Enabled but exploring: no store. */
		*enable = 1;
		*(uint32_t*)(cam + CAMERA_OBJECT_MODE_FLAGS_OFFSET) = 0x4; /* tactical only */
		assert(f(cam, Bits(45.f), 3) == 6);
		assert(*(float*)(cam + 0x164) == tagged_old);

		/* Direct method exploring: register keeps BG3's value (the jitter source). */
		assert(*(uint32_t*)(cam + 0x100) == Bits(45.f));

		/* Sync method (bit 1) exploring: no store, register gets the mod's pitch. */
		*enable = 3;
		*(uint32_t*)(cam + CAMERA_OBJECT_MODE_FLAGS_OFFSET) = 0x408;
		assert(f(cam, Bits(33.f), 5) == 10);
		assert(*(float*)(cam + 0x164) == tagged_old);
		assert(*(uint32_t*)(cam + 0x100) == Bits(tagged_old));
		*enable = 2;
		assert(f(cam, Bits(33.f), 5) == 10);
		assert(*(float*)(cam + 0x164) == tagged_old);
		assert(*(uint32_t*)(cam + 0x100) == Bits(tagged_old));
		/* Sync method without vanilla combat: combat cameras synced too. */
		*(uint32_t*)(cam + CAMERA_OBJECT_MODE_FLAGS_OFFSET) = CAMERA_MODE_COMBAT;
		assert(f(cam, Bits(33.f), 5) == 10);
		assert(*(float*)(cam + 0x164) == tagged_old);
		assert(*(uint32_t*)(cam + 0x100) == Bits(tagged_old));
		*enable = 3; /* sync + vanilla combat */

		/* Enabled and in combat: BG3's pitch, player tag kept. */
		*(uint32_t*)(cam + CAMERA_OBJECT_MODE_FLAGS_OFFSET) = CAMERA_MODE_COMBAT | 0x4;
		assert(f(cam, Bits(45.f), 0x12345) == 0x2468A);
		float stored = *(float*)(cam + 0x164);
		printf("xmm%d combat store: %.6f tag=%d\n", xmm, stored, PitchTagPlayer(cam));
		assert(fabsf(stored - 45.f) < 0.001f);
		assert(PitchTagPlayer(cam) == 1);
		assert(*(uint32_t*)(cam + 0x100) == Bits(45.f)); /* vanilla register untouched */

		munmap(stub, (size_t)page);
		munmap(code, (size_t)page);
	}
	printf("pitch stub tests passed\n");
	return 0;
}

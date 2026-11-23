/*
 * Simulates splitscreen: two controllers, many stale camera objects from
 * earlier loads, then two live cameras updated alternately.
 * Includes main.c directly to exercise the real hook and event code.
 */
#include "../src/main.c"

#include <assert.h>

#define CAM_SIZE 0x400

static int16_t fake_axis[4][SDL_CONTROLLER_AXIS_MAX];
static uint8_t fake_button[4][SDL_CONTROLLER_BUTTON_MAX];

static SDL_GameController* FakeFromInstance(SDL_JoystickID id)
{
	return (id >= 0 && id < 4) ? (SDL_GameController*)(intptr_t)(id + 1) : NULL;
}
static Sint16 FakeGetAxis(SDL_GameController* c, SDL_GameControllerAxis a)
{
	return fake_axis[(intptr_t)c - 1][a];
}
static Uint8 FakeGetButton(SDL_GameController* c, SDL_GameControllerButton b)
{
	return fake_button[(intptr_t)c - 1][b];
}
static float FakeCalc(void* cam, uint8_t angle)
{
	(void)cam; (void)angle;
	return 0.f;
}

static void Axis(int pad, int axis, int16_t value)
{
	fake_axis[pad][axis] = value;
	SDL_Event e;
	memset(&e, 0, sizeof(e));
	e.type = SDL_CONTROLLERAXISMOTION;
	e.caxis.which = pad;
	e.caxis.axis = (Uint8)axis;
	e.caxis.value = value;
	HandleSDLEvent(&e);
}

static void L3(int pad, int down)
{
	fake_button[pad][SDL_CONTROLLER_BUTTON_LEFTSTICK] = (uint8_t)down;
	SDL_Event e;
	memset(&e, 0, sizeof(e));
	e.type = down ? SDL_CONTROLLERBUTTONDOWN : SDL_CONTROLLERBUTTONUP;
	e.cbutton.which = pad;
	e.cbutton.button = SDL_CONTROLLER_BUTTON_LEFTSTICK;
	HandleSDLEvent(&e);
}

static uint8_t* NewCamera(void)
{
	uint8_t* cam = calloc(1, CAM_SIZE);
	*(float*)(cam + 0x164) = 30.f;               /* pitch */
	*(float*)(cam + 0x54) = 10.f;
	*(float*)(cam + 0x58) = 10.f;
	*(float*)(cam + 0x5C) = 10.f;                /* zoom */
	*(float*)(cam + CAMERA_OBJECT_ROTATION_SPEED_OFFSET) = 90.f;
	return cam;
}

static float Pitch(uint8_t* cam) { return *(float*)(cam + 0x164); }
static float Zoom(uint8_t* cam) { return *(float*)(cam + 0x58); }

static void Sleep10ms(void)
{
	struct timespec ts = { 0, 10000000L };
	nanosleep(&ts, NULL);
}

/* Run frames; optionally let the "game" yaw a camera each frame. */
static void Frames(int n, uint8_t* a, uint8_t* b, uint8_t* yaw_cam)
{
	for (int i = 0; i < n; i++)
	{
		(void)yaw_cam;
		H_CalculateCameraAngle_CallSite(a, 0);
		if (b)
			H_CalculateCameraAngle_CallSite(b, 0);
		Sleep10ms();
	}
}

int main(void)
{
	InitPads();
	g_config.vanilla_combat_camera = LNCT_VANILLA_COMBAT_SPLITSCREEN;
	UpdateVanillaCombatEnabled();
	assert(!*g_vanilla_combat_enabled); /* not before splitscreen was used */
	g_setup_succeeded = 1;
	O_CalculateCameraAngle = FakeCalc;
	O_SDL_GameControllerFromInstanceID = FakeFromInstance;
	O_SDL_GameControllerGetAxis = FakeGetAxis;
	O_SDL_GameControllerGetButton = FakeGetButton;

	/* --- Single player still works, using whichever pad is active. --- */
	uint8_t* solo = NewCamera();
	SDL_Event hello;
	memset(&hello, 0, sizeof(hello));
	hello.type = SDL_CONTROLLERBUTTONDOWN;
	hello.cbutton.which = 0;
	hello.cbutton.button = SDL_CONTROLLER_BUTTON_A;
	HandleSDLEvent(&hello); /* pad 0 known before the camera runs */
	Frames(2, solo, NULL, NULL);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(10, solo, NULL, NULL);
	assert(Pitch(solo) != 30.f);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	printf("single player pitch ok: %.2f\n", Pitch(solo));

	/* --- Many camera objects from earlier loads fill the table. --- */
	for (int i = 0; i < 40; i++)
	{
		uint8_t* junk = NewCamera();
		H_CalculateCameraAngle_CallSite(junk, 0);
	}
	Sleep10ms();
	Sleep10ms();
	Sleep10ms();
	struct timespec pause = { 0, 300000000L };
	nanosleep(&pause, NULL); /* let the stale cameras go inactive */

	/* --- Splitscreen: two viewport cameras, adjacent in one array. --- */
	uint8_t* block = calloc(1, SPLITSCREEN_CAMERA_STRIDE * 2 + CAM_SIZE);
	uint8_t* p1cam = block;
	uint8_t* p2cam = block + SPLITSCREEN_CAMERA_STRIDE;
	for (int i = 0; i < 2; i++)
	{
		uint8_t* cam = block + i * SPLITSCREEN_CAMERA_STRIDE;
		*(float*)(cam + 0x164) = 30.f;
		*(float*)(cam + 0x54) = 10.f;
		*(float*)(cam + 0x58) = 10.f;
		*(float*)(cam + 0x5C) = 10.f;
		*(float*)(cam + CAMERA_OBJECT_ROTATION_SPEED_OFFSET) = 90.f;
		*(float*)(cam + LNCT_CAMERA_ROOT_POS_OFFSET) = i * 12.f; /* players 12 m apart */
	}
	/* Player 2's controller connects later (higher instance id) and has only
	 * pressed a face button so far. */
	SDL_Event join;
	memset(&join, 0, sizeof(join));
	join.type = SDL_CONTROLLERBUTTONDOWN;
	join.cbutton.which = 1;
	join.cbutton.button = SDL_CONTROLLER_BUTTON_A;
	HandleSDLEvent(&join);
	Frames(3, p2cam, p1cam, NULL); /* call order must not matter */

	/* A temporary extra camera during splitscreen gets no stick input. */
	uint8_t* temp = NewCamera();
	*(float*)(temp + LNCT_CAMERA_ROOT_POS_OFFSET) = 500.f; /* unrelated view */
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(5, p1cam, temp, NULL);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	assert(Pitch(temp) == 30.f);
	nanosleep(&pause, NULL);
	Frames(2, p1cam, p2cam, NULL);

	/* Pad 0 pitch must only move p1cam. */
	float p1_before = Pitch(p1cam), p2_before = Pitch(p2cam);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(10, p1cam, p2cam, NULL);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	printf("pad0 pitch: p1 %.2f -> %.2f, p2 %.2f -> %.2f\n",
		p1_before, Pitch(p1cam), p2_before, Pitch(p2cam));
	assert(fabsf(Pitch(p1cam) - p1_before) > 0.01f);
	assert(fabsf(Pitch(p2cam) - p2_before) < 0.001f);

	/* Pad 1 drives player 2's camera only. */
	p1_before = Pitch(p1cam);
	p2_before = Pitch(p2cam);
	Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, 32767);
	Frames(10, p1cam, p2cam, NULL);
	Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	printf("pad1 pitch: p1 %.2f -> %.2f, p2 %.2f -> %.2f\n",
		p1_before, Pitch(p1cam), p2_before, Pitch(p2cam));
	assert(fabsf(Pitch(p1cam) - p1_before) < 0.001f);
	assert(fabsf(Pitch(p2cam) - p2_before) > 0.01f);

	/* Pad 1 L3 + stick zooms only p2cam, and the L3 click is swallowed. */
	float z1 = Zoom(p1cam), z2 = Zoom(p2cam);
	L3(1, 1);
	Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(10, p1cam, p2cam, NULL);
	Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	L3(1, 0);
	printf("pad1 zoom: p1 %.2f -> %.2f, p2 %.2f -> %.2f\n", z1, Zoom(p1cam), z2, Zoom(p2cam));
	assert(Zoom(p1cam) == z1);
	assert(Zoom(p2cam) != z2);
	assert(g_deferred_controller_event_count == 0);

	/* A plain L3 click on pad 0 is still forwarded to BG3: press now,
	 * release on the next frame. */
	L3(0, 1);
	L3(0, 0);
	assert(g_deferred_controller_event_count == 1);
	assert(g_deferred_controller_events[0].type == SDL_CONTROLLERBUTTONDOWN);
	assert(g_deferred_controller_events[0].cbutton.which == 0);
	assert(g_next_frame_event_count == 1);
	assert(g_next_frame_events[0].type == SDL_CONTROLLERBUTTONUP);
	g_deferred_controller_event_count = 0;
	g_deferred_controller_event_index = 0;
	/* While the replayed click is held, polling L3 reports it pressed even
	 * though the physical button is already up. */
	assert(SDL_GameControllerGetButton(FakeFromInstance(0), SDL_CONTROLLER_BUTTON_LEFTSTICK) == 1);
	PromoteNextFrameEvents(MonotonicNs());          /* too early: release not due */
	assert(g_deferred_controller_event_count == 0);
	PromoteNextFrameEvents(MonotonicNs() + L3_REPLAY_HOLD_NS + 1);
	assert(g_deferred_controller_event_count == 1);
	assert(g_deferred_controller_events[0].type == SDL_CONTROLLERBUTTONUP);
	g_deferred_controller_event_count = 0;
	atomic_store(&g_pads[0].l3_replay_until_ns, 0);
	assert(SDL_GameControllerGetButton(FakeFromInstance(0), SDL_CONTROLLER_BUTTON_LEFTSTICK) == 0);

	/* While L3 is physically held and the mod is still deciding, BG3's poll
	 * sees it released, so holding L3 to zoom never toggles the cursor. */
	L3(0, 1);
	assert(fake_button[0][SDL_CONTROLLER_BUTTON_LEFTSTICK] == 1);
	assert(SDL_GameControllerGetButton(FakeFromInstance(0), SDL_CONTROLLER_BUTTON_LEFTSTICK) == 0);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(3, p1cam, p2cam, NULL);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	L3(0, 0);
	assert(g_deferred_controller_event_count == 0); /* used for zoom: no click */
	assert(SDL_GameControllerGetButton(FakeFromInstance(0), SDL_CONTROLLER_BUTTON_LEFTSTICK) == 0);

	/* Both players pitching at the same time, independently. The two
	 * cameras report very different game rotation speeds (like Underdark vs
	 * camp); controller pitch speed must not depend on that. */
	*(float*)(p1cam + CAMERA_OBJECT_ROTATION_SPEED_OFFSET) = 30.f;
	*(float*)(p2cam + CAMERA_OBJECT_ROTATION_SPEED_OFFSET) = 300.f;
	p1_before = Pitch(p1cam);
	p2_before = Pitch(p2cam);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 32767);
	Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(10, p1cam, p2cam, NULL);
	printf("both: p1 %.2f -> %.2f, p2 %.2f -> %.2f\n",
		p1_before, Pitch(p1cam), p2_before, Pitch(p2cam));
	assert((Pitch(p1cam) - p1_before) * (Pitch(p2cam) - p2_before) < 0.f);
	{
		float d1 = fabsf(Pitch(p1cam) - p1_before), d2 = fabsf(Pitch(p2cam) - p2_before);
		printf("pitch moved %.3f vs %.3f with game rotation speeds 30 vs 300\n", d1, d2);
		assert(fabsf(d1 - d2) < 0.15f * d1);
	}

	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, 0);

	/* BG3 swaps the two viewport objects' contents: input must follow players. */
	{
		uint8_t tmp[SPLITSCREEN_CAMERA_STRIDE];
		memcpy(tmp, p1cam, SPLITSCREEN_CAMERA_STRIDE);
		memcpy(p1cam, p2cam, SPLITSCREEN_CAMERA_STRIDE);
		memcpy(p2cam, tmp, SPLITSCREEN_CAMERA_STRIDE);
	}
	/* p2cam now shows player 1. */
	p1_before = Pitch(p2cam);
	p2_before = Pitch(p1cam);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(10, p1cam, p2cam, NULL);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	printf("after swap pad0: player1 view %.2f -> %.2f, player2 view %.2f -> %.2f\n",
		p1_before, Pitch(p2cam), p2_before, Pitch(p1cam));
	assert(fabsf(Pitch(p2cam) - p1_before) > 0.01f);
	assert(fabsf(Pitch(p1cam) - p2_before) < 0.001f);

	/* Player 1's camera moves into a new, non-adjacent object. */
	uint8_t* moved = calloc(1, CAM_SIZE);
	memcpy(moved, p2cam, SPLITSCREEN_CAMERA_STRIDE);
	float z1b = Zoom(moved), z2b = Zoom(p1cam);
	L3(0, 1);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(10, moved, p1cam, NULL);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	L3(0, 0);
	printf("moved object pad0 zoom: player1 %.2f -> %.2f, player2 %.2f -> %.2f\n",
		z1b, Zoom(moved), z2b, Zoom(p1cam));
	assert(Zoom(moved) != z1b);
	assert(Zoom(p1cam) == z2b);
	{
		uint8_t* player1_view = moved;
		uint8_t* player2_view = p1cam;
		p1cam = player1_view;
		p2cam = player2_view;
	}

	/* Combat start: BG3 re-creates the pair with player 2's content first and
	 * the look-at points jump; player 1's old camera has been quiet > 1 s. */
	{
		struct timespec quiet = { 1, 200000000L };
		nanosleep(&quiet, NULL);
		uint8_t* combat = calloc(1, SPLITSCREEN_CAMERA_STRIDE * 2 + CAM_SIZE);
		memcpy(combat, p2cam, SPLITSCREEN_CAMERA_STRIDE);                      /* P2 first */
		memcpy(combat + SPLITSCREEN_CAMERA_STRIDE, p1cam, SPLITSCREEN_CAMERA_STRIDE);
		*(float*)(combat + LNCT_CAMERA_ROOT_POS_OFFSET) = 300.f;
		*(float*)(combat + SPLITSCREEN_CAMERA_STRIDE + LNCT_CAMERA_ROOT_POS_OFFSET) = 305.f;
		*(float*)(combat + 0x58) = 15.f;
		*(float*)(combat + SPLITSCREEN_CAMERA_STRIDE + 0x58) = 15.f;
		p1cam = combat + SPLITSCREEN_CAMERA_STRIDE;
		p2cam = combat;
		Frames(3, p2cam, p1cam, NULL);
		p1_before = Pitch(p1cam);
		p2_before = Pitch(p2cam);
		Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
		Frames(10, p1cam, p2cam, NULL);
		Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
		printf("combat pad0: player1 %.2f -> %.2f, player2 %.2f -> %.2f\n",
			p1_before, Pitch(p1cam), p2_before, Pitch(p2cam));
		assert(fabsf(Pitch(p1cam) - p1_before) > 0.01f);
		assert(fabsf(Pitch(p2cam) - p2_before) < 0.001f);
	}

	/* Vanilla combat: player 1 enters combat, player 2 keeps exploring. */
	assert(*g_vanilla_combat_enabled); /* splitscreen has been seen */
	{
		*(uint32_t*)(p1cam + CAMERA_OBJECT_MODE_FLAGS_OFFSET) = CAMERA_MODE_COMBAT;
		struct timespec grace = { 0, 600000000L };
		for (int i = 0; i < 60; i++)
			Frames(1, p1cam, p2cam, NULL);
		nanosleep(&grace, NULL); /* pad 0 ownership lapses */
		Frames(2, p1cam, p2cam, NULL);
		SDL_Event e;
		memset(&e, 0, sizeof(e));
		e.type = SDL_CONTROLLERAXISMOTION;
		e.caxis.which = 0;
		e.caxis.axis = SDL_CONTROLLER_AXIS_RIGHTY;
		e.caxis.value = 32767;
		assert(HandleSDLEvent(&e) == 0); /* BG3 gets player 1's stick */
		fake_axis[0][SDL_CONTROLLER_AXIS_RIGHTY] = 32767;
		float c1 = Pitch(p1cam), z1c = Zoom(p1cam);
		Frames(10, p1cam, p2cam, NULL);
		fake_axis[0][SDL_CONTROLLER_AXIS_RIGHTY] = 0;
		assert(Pitch(p1cam) == c1 && Zoom(p1cam) == z1c); /* mod leaves it alone */
		assert(PitchTagPlayer(p1cam) == 0);              /* still player 1 */

		/* Player 2, still exploring, keeps the modded camera. */
		p2_before = Pitch(p2cam);
		Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
		Frames(10, p1cam, p2cam, NULL);
		Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, 0);
		assert(fabsf(Pitch(p2cam) - p2_before) > 0.01f);

		/* Combat over: player 1 gets the mod back. */
		*(uint32_t*)(p1cam + CAMERA_OBJECT_MODE_FLAGS_OFFSET) = 0;
		Frames(2, p1cam, p2cam, NULL);
		p1_before = Pitch(p1cam);
		Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
		Frames(10, p1cam, p2cam, NULL);
		Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
		assert(fabsf(Pitch(p1cam) - p1_before) > 0.01f);
		printf("vanilla combat: player 1 untouched in combat, modded again after\n");
	}

	/* swap_splitscreen_controllers reverses the assignment. */
	g_config.swap_splitscreen_controllers = 1;
	p1_before = Pitch(p1cam);
	p2_before = Pitch(p2cam);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(10, p1cam, p2cam, NULL);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	assert(fabsf(Pitch(p1cam) - p1_before) < 0.001f);
	assert(fabsf(Pitch(p2cam) - p2_before) > 0.01f);
	g_config.swap_splitscreen_controllers = 0;

	/* Combat: only player 1's camera reaches the hook. Player 1's pad still
	 * drives it; player 2's pad must not, and its input goes to BG3 instead. */
	struct timespec leave = { 1, 100000000L };
	nanosleep(&leave, NULL);
	Frames(2, p1cam, NULL, NULL);
	p1_before = Pitch(p1cam);
	Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, 32767);
	Frames(10, p1cam, NULL, NULL);
	printf("combat pad1 on lone P1 camera: %.2f -> %.2f\n", p1_before, Pitch(p1cam));
	assert(fabsf(Pitch(p1cam) - p1_before) < 0.001f);
	{
		SDL_Event e;
		memset(&e, 0, sizeof(e));
		e.type = SDL_CONTROLLERAXISMOTION;
		e.caxis.which = 1;
		e.caxis.axis = SDL_CONTROLLER_AXIS_RIGHTY;
		e.caxis.value = 32767;
		assert(HandleSDLEvent(&e) == 0); /* vanilla for player 2 */
		SDL_Event l3;
		memset(&l3, 0, sizeof(l3));
		l3.type = SDL_CONTROLLERBUTTONDOWN;
		l3.cbutton.which = 1;
		l3.cbutton.button = SDL_CONTROLLER_BUTTON_LEFTSTICK;
		assert(HandleSDLEvent(&l3) == 0);
		l3.type = SDL_CONTROLLERBUTTONUP;
		assert(HandleSDLEvent(&l3) == 0);
	}
	Axis(1, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, -32768);
	Frames(10, p1cam, NULL, NULL);
	Axis(0, SDL_CONTROLLER_AXIS_RIGHTY, 0);
	printf("combat pad0 on lone P1 camera: %.2f -> %.2f\n", p1_before, Pitch(p1cam));
	assert(fabsf(Pitch(p1cam) - p1_before) > 0.01f);
	assert(PitchTagPlayer(p1cam) == 0);

	/* Pitch written for an untagged single-player camera never forms a tag. */
	for (uint32_t low = 0; low < 64; low++)
	{
		float f = 30.f;
		uint32_t bits;
		memcpy(&bits, &f, 4);
		bits = (bits & ~0x3Fu) | low;
		memcpy(&f, &bits, 4);
		f = UntagPitch(f);
		memcpy(&bits, &f, 4);
		assert((bits & 0x3Fu) != 0x15u && (bits & 0x3Fu) != 0x2Au);
	}

	printf("splitscreen tests passed\n");
	return 0;
}

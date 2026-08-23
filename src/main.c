#include <dlfcn.h>
#include <sys/types.h>
#include <stdatomic.h>
#include <time.h>
#include <wchar.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "camera_state.h"
#include "controller_input.h"
#include "portable_paths.h"
#include "sdl_bindings.h"
#include "utils.h"
#include "offsets.h"


#define VERSION "1.0.21-controller-input-fix"
//Tested Game build:
	//4.1.1.7398727
	//4.1.1.7209685

#define MOUSE_ZOOM_FACTOR 0.25f

#define CAMERA_OBJECT_ROTATION_SPEED_OFFSET 0xC4
#define MAX_CAMERA_DELTA_TIME 0.1f
#define MAX_TRACKED_CAMERAS 4


static pid_t g_pid = 0;
static int g_setup_succeeded;
static uint64_t g_game_build;

static float* g_roll;

static atomic_int g_mouse_delta_y;
static atomic_int g_mouse_wheel_y;
static atomic_int g_roll_keydown = 0;
static atomic_int g_roll_input_sources = 0;
static atomic_int g_mod_owns_relative_mouse_mode = 0;
static atomic_int g_ignore_next_mouse_motion = 0;
static atomic_int g_controller_right_stick_y;
static atomic_int g_controller_right_stick_axis_motion_y = 0;
static atomic_int g_controller_left_stick_button_down = 0;
static atomic_int g_controller_left_stick_used_for_zoom = 0;
static atomic_int g_controller_instance_id = -1;

static SDL_Event g_left_stick_down_event;
static int g_has_left_stick_down_event;
static SDL_Event g_deferred_controller_events[2];
static int g_deferred_controller_event_count;
static int g_deferred_controller_event_index;

static LNCT_Config g_config =
{
	.controller_pitch_sensitivity = LNCT_DEFAULT_CONTROLLER_PITCH_SENSITIVITY,
	.controller_zoom_speed = LNCT_DEFAULT_CONTROLLER_ZOOM_SPEED,
	.mouse_pitch_sensitivity = LNCT_DEFAULT_MOUSE_PITCH_SENSITIVITY,
	.invert_controller_pitch = 0,
};
static char g_config_path[1024];

typedef struct
{
	void* camera_object;
	struct timespec last_update;
} CameraTiming;
static CameraTiming g_camera_timings[MAX_TRACKED_CAMERAS];

static BindingSet g_bs;
static const ActionBindings* g_binds[1];
static ActionBindings g_default_roll_binding;
static char g_input_config_path[1200];
	 
static int (*O_PollEvent)(SDL_Event*) = NULL;
static SDL_bool (*O_SDL_GetRelativeMouseMode)(void) = NULL;
static int (*O_SDL_SetRelativeMouseMode)(SDL_bool) = NULL;
static SDL_GameController* (*O_SDL_GameControllerFromInstanceID)(SDL_JoystickID) = NULL;
static Sint16 (*O_SDL_GameControllerGetAxis)(SDL_GameController*, SDL_GameControllerAxis) = NULL;
static Uint8 (*O_SDL_GameControllerGetButton)(SDL_GameController*, SDL_GameControllerButton) = NULL;

#if defined(__GLIBC__)
extern void* LNCT_DlsymCompat(void*, const char*);
__asm__(".symver LNCT_DlsymCompat,dlsym@GLIBC_2.2.5");
#else
#define LNCT_DlsymCompat dlsym
#endif

typedef float (*CalculateCameraAngle_t)(void*, uint8_t);
static CalculateCameraAngle_t O_CalculateCameraAngle;

enum
{
	ROLL_INPUT_MOUSE = 1,
	ROLL_INPUT_KEYBOARD = 2,
};


static void SetRollInputSource(int source, int active)
{
	if (active)
	{
		int previous_sources = atomic_fetch_or(&g_roll_input_sources, source);
		if (previous_sources != 0)
			return;

		atomic_store(&g_roll_keydown, 1);
		if (O_SDL_GetRelativeMouseMode && O_SDL_SetRelativeMouseMode
			&& O_SDL_GetRelativeMouseMode() != SDL_TRUE
			&& O_SDL_SetRelativeMouseMode(SDL_TRUE) == 0)
		{
			atomic_store(&g_mod_owns_relative_mouse_mode, 1);
			atomic_store(&g_ignore_next_mouse_motion, 1);
		}
		/* Enabling relative mode can itself create a synthetic motion event. */
		atomic_store(&g_mouse_delta_y, 0);
		return;
	}

	int previous_sources = atomic_fetch_and(&g_roll_input_sources, ~source);
	if ((previous_sources & ~source) != 0)
		return;

	atomic_store(&g_roll_keydown, 0);
	atomic_store(&g_mouse_delta_y, 0);
	atomic_store(&g_ignore_next_mouse_motion, 0);
	if (atomic_exchange(&g_mod_owns_relative_mouse_mode, 0)
		&& O_SDL_SetRelativeMouseMode)
	{
		O_SDL_SetRelativeMouseMode(SDL_FALSE);
	}
}

static void ResetInputState(void)
{
	atomic_store(&g_roll_input_sources, 0);
	atomic_store(&g_roll_keydown, 0);
	atomic_store(&g_mouse_delta_y, 0);
	atomic_store(&g_mouse_wheel_y, 0);
	atomic_store(&g_ignore_next_mouse_motion, 0);
	if (atomic_exchange(&g_mod_owns_relative_mouse_mode, 0)
		&& O_SDL_SetRelativeMouseMode)
	{
		O_SDL_SetRelativeMouseMode(SDL_FALSE);
	}
}

static void ResetControllerState(void)
{
	atomic_store(&g_controller_right_stick_y, 0);
	atomic_store(&g_controller_right_stick_axis_motion_y, 0);
	atomic_store(&g_controller_left_stick_button_down, 0);
	atomic_store(&g_controller_left_stick_used_for_zoom, 0);
}

/*
 * SDL axis/button events describe transitions, not an authoritative snapshot.
 * If a release/centering event is lost during a focus or input-mode transition,
 * the old sample otherwise remains active forever.  Reconcile it with SDL's
 * live controller state on each camera update when the query API is available.
 */
static void RefreshControllerState(void)
{
	if (!O_SDL_GameControllerFromInstanceID
		|| !O_SDL_GameControllerGetAxis
		|| !O_SDL_GameControllerGetButton)
	{
		return;
	}

	SDL_JoystickID instance_id = (SDL_JoystickID)atomic_load(&g_controller_instance_id);
	if (instance_id < 0)
		return;

	SDL_GameController* controller = O_SDL_GameControllerFromInstanceID(instance_id);
	if (!controller)
		return;

	int16_t right_stick_y = O_SDL_GameControllerGetAxis(
		controller, SDL_CONTROLLER_AXIS_RIGHTY);
	int left_stick_down = O_SDL_GameControllerGetButton(
		controller, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
	int axis_active = LNCT_NormalizeControllerAxis(right_stick_y) != 0.f;

	atomic_store(&g_controller_right_stick_y, right_stick_y);
	atomic_store(&g_controller_right_stick_axis_motion_y, axis_active);
	atomic_store(&g_controller_left_stick_button_down, left_stick_down);
	if (left_stick_down && axis_active)
		atomic_store(&g_controller_left_stick_used_for_zoom, 1);
}

static void UseDefaultRollBinding(void)
{
	memset(&g_default_roll_binding, 0, sizeof(g_default_roll_binding));
	snprintf(g_default_roll_binding.name, sizeof(g_default_roll_binding.name),
		"CameraToggleMouseRotate");
	g_default_roll_binding.binding_count = 1;
	g_default_roll_binding.bindings[0].type = BINDING_MOUSE;
	g_default_roll_binding.bindings[0].scancode = SDL_SCANCODE_UNKNOWN;
	g_default_roll_binding.bindings[0].mouse_button = SDL_BUTTON_MIDDLE;
	snprintf(g_default_roll_binding.bindings[0].raw,
		sizeof(g_default_roll_binding.bindings[0].raw), "middle");
	g_binds[0] = &g_default_roll_binding;
}

static void ReloadRollBindings(void)
{
	if (LNCT_FindInputConfigPath(g_input_config_path, sizeof(g_input_config_path))
		&& LoadBindingsFromFile(g_input_config_path, &g_bs))
	{
		const ActionBindings* bindings = FindAction(&g_bs, "CameraToggleMouseRotate");
		if (bindings && bindings->binding_count > 0)
		{
			g_binds[0] = bindings;
			fprintf(stdout, "\e[1;95m[LNCT]\e[0m Mouse-rotate bindings: %s\n",
				g_input_config_path);
			return;
		}
	}

	UseDefaultRollBinding();
	fprintf(stderr,
		"\e[1;95m[LNCT]\e[0m WARN: BG3 input config not found or missing CameraToggleMouseRotate; using middle mouse button\n");
}


static float GetCameraDeltaTime(void* camera_object)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0.f;

	CameraTiming* free_slot = NULL;
	for (int i = 0; i < MAX_TRACKED_CAMERAS; i++)
	{
		CameraTiming* timing = &g_camera_timings[i];
		if (!timing->camera_object && !free_slot)
			free_slot = timing;
		if (timing->camera_object != camera_object)
			continue;

		float delta_time = (float)(now.tv_sec - timing->last_update.tv_sec)
			+ (float)(now.tv_nsec - timing->last_update.tv_nsec) / 1000000000.f;
		timing->last_update = now;

		if (delta_time < 0.f)
			return 0.f;
		if (delta_time > MAX_CAMERA_DELTA_TIME)
			return MAX_CAMERA_DELTA_TIME;
		return delta_time;
	}

	/* A new camera gets a timing baseline; movement starts on its next update. */
	CameraTiming* timing = free_slot ? free_slot : &g_camera_timings[0];
	timing->camera_object = camera_object;
	timing->last_update = now;
	return 0.f;
}


float H_CalculateCameraAngle_CallSite(void* pCameraObject, uint8_t angle)
{
	RefreshControllerState();
	float delta_time = GetCameraDeltaTime(pCameraObject);
	int roll_keydown = atomic_load(&g_roll_keydown);
	int left_stick_button_down = atomic_load(&g_controller_left_stick_button_down);
	int right_stick_axis_motion_y = atomic_load(&g_controller_right_stick_axis_motion_y);
	if (!roll_keydown && left_stick_button_down && right_stick_axis_motion_y)
	{
		int16_t right_stick_y = (int16_t)atomic_load(&g_controller_right_stick_y);
		LNCT_ApplyZoomDelta(pCameraObject,
			LNCT_ControllerZoomDelta(right_stick_y, delta_time, g_config.controller_zoom_speed));
		atomic_store(&g_controller_left_stick_used_for_zoom, 1);
		return O_CalculateCameraAngle(pCameraObject, angle);
	}
	else
	{
		int zoom = atomic_exchange(&g_mouse_wheel_y, 0);
		/* A zero delta must not overwrite BG3's in-progress zoom interpolation. */
		if (zoom != 0)
			LNCT_ApplyZoomDelta(pCameraObject, -((float)zoom * MOUSE_ZOOM_FACTOR));
	}

	if (!roll_keydown && !right_stick_axis_motion_y)
		return O_CalculateCameraAngle(pCameraObject, angle);

	g_roll = (float*)((uint8_t*)pCameraObject + 0x164);
	float roll = *g_roll;

	/* An explicitly held mouse-rotate binding wins over stale controller state. */
	if (right_stick_axis_motion_y && !roll_keydown)
	{
		int16_t value = (int16_t)atomic_load(&g_controller_right_stick_y);
		float rotation_speed = LNCT_StableRotationSpeed(
			*(float*)((uint8_t*)pCameraObject + CAMERA_OBJECT_ROTATION_SPEED_OFFSET));
		float pitch_delta = LNCT_ControllerPitchDelta(value, delta_time, rotation_speed)
			* g_config.controller_pitch_sensitivity;
		roll += g_config.invert_controller_pitch ? -pitch_delta : pitch_delta;
	}
	else
	{
		int delta = atomic_exchange(&g_mouse_delta_y, 0);
		roll += (float)delta * g_config.mouse_pitch_sensitivity;
	}

	if (roll > 89.f)
		roll = 89.f;
	else
		roll = roll;
	if (roll < -89.f)
		roll = -89.f;
	else
		roll = roll;
	*g_roll = roll;

	return O_CalculateCameraAngle(pCameraObject, angle);
}

uint8_t PatchUpdateCamera()
{
	void* movss = (void*)GetAddresses()->roll_movss;
	long page_size = sysconf(_SC_PAGESIZE);
	uint64_t page_start = (uint64_t)movss & ~(page_size - 1);
	size_t num_pages = (((uint64_t)movss + 8 - page_start) + page_size - 1) / page_size;
	if (num_pages < 1)
		num_pages = 1;

	if (mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
	{
		fprintf(stdout, "\e[1;95m[LNCT]\e[0m ERR: PatchUpdateCamera(): roll movss mprotect() failed\n");
		return 0;
	}

	uint8_t nop[8] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
	memcpy(movss, nop, 8);
	mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_EXEC);

	movss = (void*)GetAddresses()->zoom_movss;
	page_start = (uint64_t)movss & ~(page_size - 1);
	num_pages = (((uint64_t)movss + 6 - page_start) + page_size - 1) / page_size;
	if (num_pages < 1)
		num_pages = 1;

	if (mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
	{
		fprintf(stdout, "\e[1;95m[LNCT]\e[0m ERR: PatchUpdateCamera(): zoom movss mprotect() failed\n");
		return 0;
	}

	memcpy(movss, nop, 6);
	mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_EXEC);

	return 1;
}

uint8_t SetupCallSitesTrampoline()
{
	size_t page_size = sysconf(_SC_PAGESIZE);

	void* callsite = (void*)GetAddresses()->CalculateCameraAngle_CallSite;
	void* trampoline = AllocNear(callsite, page_size);
	if (!trampoline)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: SetupCallSitesTrampoline(): AllocNear() failed for CalculateCameraAngle()\n");
		return 0;
	}

	if (!PatchCallSite(callsite, trampoline, H_CalculateCameraAngle_CallSite))
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: SetupCallSitesTrampoline(): PatchCallSite() failed for CalculateCameraAngle()\n");
		munmap(trampoline, page_size);
		return 0;
	}

	return 1;
}

uint64_t FNV1a_Hash(const uint8_t* data, size_t len)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++)
    {
        hash ^= data[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

uint64_t GetBuild()
{
	size_t size = 0;
	uint8_t* file = MapSelfExe(&size);
	if (!file || !size)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Couldn't read the running BG3 executable\n");
		return 0;
	}
    uint64_t hash = FNV1a_Hash(file, size);
    fprintf(stdout, "[LNCT] Binary FNV1a hash: %#lx (size: %zu)\n", hash, size);
    munmap(file, size);
	switch (hash)
	{
		case 0x59b4428151c778e5:
			return 4117209685;
		case 0x142d91e7bfe067cb:
			return 4117398727;
		default:
			return 0;
	}
}


void Setup()
{
	g_pid = getpid();
	g_setup_succeeded = 0;

	fprintf(stdout, "\e[1;95m[LNCT]\e[0m \e[1;37mLinux Native Camera Tweaks %s\e[0m\n", VERSION);
	fprintf(stdout, "\e[1;95m[LNCT]\e[0m Bug(s) ? Suggestion(s) ? Add me on discord: biiinks78\n");
	g_game_build = GetBuild();
	if (!g_game_build)
	{
		fprintf(stderr,
			"\e[1;95m[LNCT]\e[0m ERR: Unsupported BG3 executable; mod disabled without patching the game\n");
		return;
	}
	fprintf(stdout, "\e[1;95m[LNCT]\e[0m Supported BG3 build detected: %lu\n", g_game_build);
	if (LNCT_LoadConfig(&g_config, g_config_path, sizeof(g_config_path)))
	{
		fprintf(stdout,
			"\e[1;95m[LNCT]\e[0m Config: %s (controller pitch %.3f, controller zoom %.3f, mouse pitch %.3f, controller inverted %s)\n",
			g_config_path,
			g_config.controller_pitch_sensitivity,
			g_config.controller_zoom_speed,
			g_config.mouse_pitch_sensitivity,
			g_config.invert_controller_pitch ? "yes" : "no");
	}
	else
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: Couldn't load or create controller config; using defaults\n");
	}

	ReloadRollBindings();

	struct Sigs* sigs = GetSigs();
	struct Addresses* addresses = GetAddresses();
	addresses->CalculateCameraAngle_CallSite = PatternScanSection(sigs->CalculateCameraAngle_Callsite, ".text") + 39;
	if (addresses->CalculateCameraAngle_CallSite == 39)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): Pattern scan failed : CalculateCameraAngle_CallSite\ngame update broke the pattern\n");
		return;
	}
	addresses->roll_movss = PatternScanSection(sigs->roll_movss, ".text");
	if (!addresses->roll_movss)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): Pattern scan failed : roll_movss\ngame update broke the pattern\n");
		return;
	}
	addresses->zoom_movss = PatternScanSection(sigs->zoom_movss, ".text");
	if (!addresses->zoom_movss)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): Pattern scan failed : zoom_movss\ngame update broke the pattern\n");
		return;
	}

	O_CalculateCameraAngle = ResolveCallTarget((void*)(GetAddresses()->CalculateCameraAngle_CallSite));
	if (!O_CalculateCameraAngle)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Couldn't resolve the original BG3 camera function\n");
		return;
	}

	if (!PatchUpdateCamera())
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): PatchUpdateCamera() failed\n");
		return;
	}

	if (!SetupCallSitesTrampoline())
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): SetupCallSitesTrampoline() failed\n");
		return;
	}

	g_setup_succeeded = 1;
	fprintf(stdout, "\e[1;95m[LNCT]\e[0m \e[1;92mEverything has been initialized correctly, enjoy ;)\e[0m\n");
}

void ResolveSDLSym()
{
	O_PollEvent = (int(*)(SDL_Event*))(intptr_t)LNCT_DlsymCompat(RTLD_NEXT, "SDL_PollEvent");
	O_SDL_GetRelativeMouseMode = (SDL_bool(*)(void))(intptr_t)LNCT_DlsymCompat(RTLD_NEXT, "SDL_GetRelativeMouseMode");
	O_SDL_SetRelativeMouseMode = (int(*)(SDL_bool))(intptr_t)LNCT_DlsymCompat(RTLD_NEXT, "SDL_SetRelativeMouseMode");
	O_SDL_GameControllerFromInstanceID = (SDL_GameController*(*)(SDL_JoystickID))(intptr_t)
		LNCT_DlsymCompat(RTLD_NEXT, "SDL_GameControllerFromInstanceID");
	O_SDL_GameControllerGetAxis = (Sint16(*)(SDL_GameController*, SDL_GameControllerAxis))(intptr_t)
		LNCT_DlsymCompat(RTLD_NEXT, "SDL_GameControllerGetAxis");
	O_SDL_GameControllerGetButton = (Uint8(*)(SDL_GameController*, SDL_GameControllerButton))(intptr_t)
		LNCT_DlsymCompat(RTLD_NEXT, "SDL_GameControllerGetButton");
}

/* Return non-zero when the event is handled by the mod and hidden from BG3. */
static int HandleSDLEvent(const SDL_Event* event)
{
	if (event->type == SDL_MOUSEMOTION)
	{
		if (atomic_exchange(&g_ignore_next_mouse_motion, 0))
			return 0;
		if (atomic_load(&g_roll_keydown))
			atomic_fetch_add(&g_mouse_delta_y, event->motion.yrel);
	}
	else if (event->type == SDL_MOUSEWHEEL)
	{
		atomic_fetch_add(&g_mouse_wheel_y, event->wheel.y);
		return 1;
	}
	else if (event->type == SDL_MOUSEBUTTONDOWN)
	{
		if (g_binds[0])
		{
			for (int i = 0; i < g_binds[0]->binding_count; i++)
			{
				const Binding* b = &g_binds[0]->bindings[i];
				if ((b->type == BINDING_MOUSE) && event->button.button == b->mouse_button)
					SetRollInputSource(ROLL_INPUT_MOUSE, 1);
			}
		}
	}
	else if (event->type == SDL_MOUSEBUTTONUP)
	{
		if (g_binds[0])
		{
			for (int i = 0; i < g_binds[0]->binding_count; i++)
			{
				const Binding* b = &g_binds[0]->bindings[i];
				if ((b->type == BINDING_MOUSE) && event->button.button == b->mouse_button)
					SetRollInputSource(ROLL_INPUT_MOUSE, 0);
			}
		}
	}
	else if (event->type == SDL_KEYDOWN)
	{
		if (g_binds[0])
		{
			for (int i = 0; i < g_binds[0]->binding_count; i++)
			{
				const Binding* b = &g_binds[0]->bindings[i];
				if ((b->type == BINDING_KEY) && event->key.keysym.scancode == b->scancode)
					SetRollInputSource(ROLL_INPUT_KEYBOARD, 1);
			}
		}
	}
	else if (event->type == SDL_KEYUP)
	{
		if (g_binds[0])
		{
			for (int i = 0; i < g_binds[0]->binding_count; i++)
			{
				const Binding* b = &g_binds[0]->bindings[i];
				if ((b->type == BINDING_KEY) && event->key.keysym.scancode == b->scancode)
					SetRollInputSource(ROLL_INPUT_KEYBOARD, 0);
			}
		}
	}
	else if ((event->type == SDL_CONTROLLERAXISMOTION) && event->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY)
	{
		int16_t value = event->caxis.value;
		int axis_active = LNCT_NormalizeControllerAxis(value) != 0.f;
		atomic_store(&g_controller_instance_id, event->caxis.which);
		atomic_store(&g_controller_right_stick_y, value);
		atomic_store(&g_controller_right_stick_axis_motion_y, axis_active);
		if (axis_active && atomic_load(&g_controller_left_stick_button_down))
			atomic_store(&g_controller_left_stick_used_for_zoom, 1);
		return 1;
	}
	else if ((event->type == SDL_CONTROLLERBUTTONDOWN) && event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK)
	{
		atomic_store(&g_controller_instance_id, event->cbutton.which);
		atomic_store(&g_controller_left_stick_button_down, 1);
		atomic_store(&g_controller_left_stick_used_for_zoom,
			atomic_load(&g_controller_right_stick_axis_motion_y));
		g_left_stick_down_event = *event;
		g_has_left_stick_down_event = 1;
		return 1;
	}
	else if ((event->type == SDL_CONTROLLERBUTTONUP) && event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK)
	{
		atomic_store(&g_controller_instance_id, event->cbutton.which);
		int used_for_zoom = atomic_exchange(&g_controller_left_stick_used_for_zoom, 0);
		atomic_store(&g_controller_left_stick_button_down, 0);
		if (!used_for_zoom && g_has_left_stick_down_event)
		{
			g_deferred_controller_events[0] = g_left_stick_down_event;
			g_deferred_controller_events[1] = *event;
			g_deferred_controller_event_count = 2;
			g_deferred_controller_event_index = 0;
		}
		g_has_left_stick_down_event = 0;
		return 1;
	}
	else if (event->type == SDL_CONTROLLERDEVICEREMOVED
		|| (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_LOST))
	{
		ResetControllerState();
		atomic_store(&g_controller_instance_id, -1);
		ResetInputState();
		g_has_left_stick_down_event = 0;
		g_deferred_controller_event_count = 0;
		g_deferred_controller_event_index = 0;
	}

	return 0;
}

static int PopDeferredControllerEvent(SDL_Event* event)
{
	if (g_deferred_controller_event_index >= g_deferred_controller_event_count)
		return 0;

	*event = g_deferred_controller_events[g_deferred_controller_event_index++];
	if (g_deferred_controller_event_index >= g_deferred_controller_event_count)
	{
		g_deferred_controller_event_count = 0;
		g_deferred_controller_event_index = 0;
	}
	return 1;
}

int SDL_PollEvent(SDL_Event* event)
{
	if (!g_pid)
		Setup();

	if (!O_PollEvent)
		ResolveSDLSym();
	if (!O_PollEvent)
		return 0;
	if (!g_setup_succeeded)
		return O_PollEvent(event);

	/* Preserve SDL_PollEvent(NULL)'s queue-check semantics. */
	if (!event)
	{
		if (g_deferred_controller_event_index < g_deferred_controller_event_count)
			return 1;
		return O_PollEvent(NULL);
	}
	if (PopDeferredControllerEvent(event))
		return 1;

	int ret;
	while ((ret = O_PollEvent(event)) != 0)
	{
		/*
		 * Never return 0 merely because the mod consumed one event: BG3 uses
		 * SDL's conventional while(SDL_PollEvent(...)) loop, where 0 means the
		 * entire queue is empty.  Continue to the next queued event instead.
		 */
		if (HandleSDLEvent(event))
		{
			if (PopDeferredControllerEvent(event))
				return 1;
			continue;
		}
		return ret;
	}

	return ret;
}

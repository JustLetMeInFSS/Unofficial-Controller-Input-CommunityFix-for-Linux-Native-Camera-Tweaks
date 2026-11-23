#include <dlfcn.h>
#include <sys/types.h>
#include <stdatomic.h>
#include <time.h>
#include <wchar.h>
#include <SDL2/SDL.h>

#include "config.h"
#include "camera_state.h"
#include "controller_input.h"
#include "file_stamp.h"
#include "portable_paths.h"
#include "sdl_bindings.h"
#include "utils.h"
#include "offsets.h"
#include "lnct_debug.h"


#define VERSION "1.0.22-community-input-fix+splitscreen.10"
//Tested Game build:
	//4.1.1.7398727
	//4.1.1.7209685

#define MOUSE_ZOOM_FACTOR 0.25f

#define CAMERA_OBJECT_ROTATION_SPEED_OFFSET 0xC4
/* CameraModeFlags (Windows 2.4.5 layout): bit 0 = combat, bit 2 = tactical. */
#define CAMERA_OBJECT_MODE_FLAGS_OFFSET 0xA8
#define CAMERA_MODE_COMBAT 0x1u
#define MAX_CAMERA_DELTA_TIME 0.1f
#define MAX_TRACKED_CAMERAS 16
#define MAX_TRACKED_PADS 4
#define CAMERA_ACTIVE_WINDOW_NS 250000000L
/* BG3 keeps the splitscreen viewport cameras in one array: player 1's camera,
 * then player 2's, 0x258 bytes apart (observed on Linux build 4.1.1). */
#define SPLITSCREEN_CAMERA_STRIDE 0x258
#define DEFERRED_EVENT_CAPACITY 8
#define BINDING_RELOAD_INTERVAL_NS 500000000L


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
/*
 * Per-controller state. Splitscreen has one controller per player, so stick
 * and L3 state must never be shared between controllers.
 * Slots are allocated by the event thread; the camera hook only reads ids.
 */
typedef struct
{
	atomic_int instance_id; /* -1 = free slot */
	atomic_int right_stick_y;
	atomic_int right_stick_y_active;
	atomic_int right_stick_x;
	atomic_int left_stick_button_down;
	atomic_int left_stick_used_for_zoom;
	/* Monotonic ns when a camera last selected this pad (camera thread). */
	atomic_llong camera_owned_ns;
	/* Event-thread only. */
	SDL_Event left_stick_down_event;
	int has_left_stick_down_event;
	int l3_passthrough; /* this L3 press went to BG3 untouched */
	/* What BG3 sees when it polls L3 (any thread). */
	atomic_int l3_held_by_mod;        /* physically held, mod still deciding: report up */
	atomic_llong l3_replay_until_ns;  /* replayed click: report down until then */
} PadState;
/* How long a replayed L3 click is held, as BG3 sees it. */
#define L3_REPLAY_HOLD_NS 120000000LL
/* A pad with no camera of its own for this long gets vanilla stick Y and L3. */
#define PAD_CAMERA_GRACE_NS 500000000LL
static PadState g_pads[MAX_TRACKED_PADS];
static atomic_int g_last_active_pad = -1;

static SDL_Event g_deferred_controller_events[DEFERRED_EVENT_CAPACITY];
static int g_deferred_controller_event_count;
static int g_deferred_controller_event_index;
/* Replayed L3 releases wait until their release time (a real, short press). */
static SDL_Event g_next_frame_events[DEFERRED_EVENT_CAPACITY];
static long long g_next_frame_release_ns[DEFERRED_EVENT_CAPACITY];
static int g_next_frame_event_count;

static long long MonotonicNs(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	return (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
}

static void PromoteNextFrameEvents(long long now_ns)
{
	int kept = 0;
	for (int i = 0; i < g_next_frame_event_count; i++)
	{
		if (g_next_frame_release_ns[i] <= now_ns
			&& g_deferred_controller_event_count < DEFERRED_EVENT_CAPACITY)
		{
			g_deferred_controller_events[g_deferred_controller_event_count++] = g_next_frame_events[i];
		}
		else
		{
			g_next_frame_events[kept] = g_next_frame_events[i];
			g_next_frame_release_ns[kept] = g_next_frame_release_ns[i];
			kept++;
		}
	}
	g_next_frame_event_count = kept;
}

static LNCT_Config g_config =
{
	.controller_pitch_sensitivity = LNCT_DEFAULT_CONTROLLER_PITCH_SENSITIVITY,
	.controller_zoom_speed = LNCT_DEFAULT_CONTROLLER_ZOOM_SPEED,
	.mouse_pitch_sensitivity = LNCT_DEFAULT_MOUSE_PITCH_SENSITIVITY,
	.invert_controller_pitch = 0,
	.invert_controller_zoom = LNCT_DEFAULT_INVERT_CONTROLLER_ZOOM,
};
static char g_config_path[1024];

/*
 * Per-camera state, touched only by the camera hook's thread.
 * BG3 creates new camera objects on every load and when a splitscreen player
 * joins, so stale entries must be recycled. (The original fixed table never
 * freed slots: once full, every new camera fought over slot 0 and always got
 * delta_time = 0, which silently disabled pitch and L3 zoom in splitscreen.)
 */
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
static LNCT_FileStamp g_input_config_stamp;
static struct timespec g_last_binding_check;
	 
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

/*
 * Vanilla combat camera. Read by the pitch-store stub inside BG3's code, so it
 * lives in the stub's own page (see PatchUpdateCamera); this points at it.
 */
static volatile uint8_t g_vanilla_combat_fallback;
static volatile uint8_t* g_vanilla_combat_enabled = &g_vanilla_combat_fallback;
static int g_splitscreen_seen;

/* Stub control byte: bit 0 = vanilla pitch in combat, bit 1 = sync BG3's register. */
#define STUB_VANILLA_COMBAT 0x1u
#define STUB_SYNC_REGISTER 0x2u

static void UpdateVanillaCombatEnabled(void)
{
	int enabled = g_config.vanilla_combat_camera == LNCT_VANILLA_COMBAT_ALWAYS
		|| (g_config.vanilla_combat_camera == LNCT_VANILLA_COMBAT_SPLITSCREEN && g_splitscreen_seen);
	uint8_t bits = enabled ? STUB_VANILLA_COMBAT : 0;
	if (g_config.pitch_method == LNCT_PITCH_SYNC)
		bits |= STUB_SYNC_REGISTER;
	*g_vanilla_combat_enabled = bits;
}

static int CameraInVanillaCombat(const void* camera_object)
{
	uint32_t flags;
	memcpy(&flags, (const uint8_t*)camera_object + CAMERA_OBJECT_MODE_FLAGS_OFFSET, sizeof(flags));
	return (flags & CAMERA_MODE_COMBAT) && (*g_vanilla_combat_enabled & STUB_VANILLA_COMBAT);
}


static void ResetPadInput(PadState* pad)
{
	atomic_store(&pad->right_stick_y, 0);
	atomic_store(&pad->right_stick_y_active, 0);
	atomic_store(&pad->right_stick_x, 0);
	atomic_store(&pad->left_stick_button_down, 0);
	atomic_store(&pad->left_stick_used_for_zoom, 0);
	pad->has_left_stick_down_event = 0;
	pad->l3_passthrough = 0;
	atomic_store(&pad->l3_held_by_mod, 0);
	atomic_store(&pad->l3_replay_until_ns, 0);
}

static void ResetControllerState(void)
{
	for (int i = 0; i < MAX_TRACKED_PADS; i++)
		ResetPadInput(&g_pads[i]);
}

static void InitPads(void)
{
	for (int i = 0; i < MAX_TRACKED_PADS; i++)
	{
		atomic_store(&g_pads[i].instance_id, -1);
		ResetPadInput(&g_pads[i]);
	}
}

/* Event thread: find (or allocate) the slot for an SDL instance id. */
static int PadSlotForInstance(SDL_JoystickID id, int create)
{
	for (int i = 0; i < MAX_TRACKED_PADS; i++)
		if (atomic_load(&g_pads[i].instance_id) == id)
			return i;
	if (!create)
		return -1;
	for (int i = 0; i < MAX_TRACKED_PADS; i++)
	{
		if (atomic_load(&g_pads[i].instance_id) < 0)
		{
			ResetPadInput(&g_pads[i]);
			atomic_store(&g_pads[i].instance_id, id);
			if (LNCT_DbgOn())
				fprintf(stderr, "[LNCT-DBG] controller instance %d -> pad slot %d\n", id, i);
			return i;
		}
	}
	return -1;
}

static void RemovePad(SDL_JoystickID id)
{
	int slot = PadSlotForInstance(id, 0);
	if (slot < 0)
		return;
	ResetPadInput(&g_pads[slot]);
	atomic_store(&g_pads[slot].instance_id, -1);
	atomic_compare_exchange_strong(&g_last_active_pad, &slot, -1);
}

static int PadValid(int slot)
{
	return slot >= 0 && slot < MAX_TRACKED_PADS
		&& atomic_load(&g_pads[slot].instance_id) >= 0;
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

	for (int i = 0; i < MAX_TRACKED_PADS; i++)
	{
		PadState* pad = &g_pads[i];
		SDL_JoystickID instance_id = (SDL_JoystickID)atomic_load(&pad->instance_id);
		if (instance_id < 0)
			continue;

		SDL_GameController* controller = O_SDL_GameControllerFromInstanceID(instance_id);
		if (!controller)
			continue;

		int16_t right_stick_y = O_SDL_GameControllerGetAxis(
			controller, SDL_CONTROLLER_AXIS_RIGHTY);
		int16_t right_stick_x = O_SDL_GameControllerGetAxis(
			controller, SDL_CONTROLLER_AXIS_RIGHTX);
		int left_stick_down = O_SDL_GameControllerGetButton(
			controller, SDL_CONTROLLER_BUTTON_LEFTSTICK) != 0;
		int axis_active = LNCT_NormalizeControllerAxis(right_stick_y) != 0.f;

		if (i == atomic_load(&g_last_active_pad))
			LNCT_DbgRefresh(instance_id, right_stick_y, left_stick_down);
		atomic_store(&pad->right_stick_y, right_stick_y);
		atomic_store(&pad->right_stick_y_active, axis_active);
		atomic_store(&pad->right_stick_x, right_stick_x);
		atomic_store(&pad->left_stick_button_down, left_stick_down);
		if (left_stick_down && axis_active)
			atomic_store(&pad->left_stick_used_for_zoom, 1);
	}
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

static int ReloadRollBindings(void)
{
	char discovered_path[sizeof(g_input_config_path)];
	BindingSet new_bindings;
	if (LNCT_FindInputConfigPath(discovered_path, sizeof(discovered_path))
		&& LoadBindingsFromFile(discovered_path, &new_bindings))
	{
		snprintf(g_input_config_path, sizeof(g_input_config_path), "%s", discovered_path);
		LNCT_ReadFileStamp(g_input_config_path, &g_input_config_stamp);
		const ActionBindings* bindings = FindAction(&new_bindings, "CameraToggleMouseRotate");
		if (bindings && bindings->binding_count > 0)
		{
			g_bs = new_bindings;
			g_binds[0] = FindAction(&g_bs, "CameraToggleMouseRotate");
			fprintf(stdout, "\e[1;95m[LNCT]\e[0m Mouse-rotate bindings: %s\n",
				g_input_config_path);
			return 1;
		}
		UseDefaultRollBinding();
		fprintf(stdout, "\e[1;95m[LNCT]\e[0m CameraToggleMouseRotate uses BG3 default: middle mouse\n");
		return 1;
	}

	if (!g_binds[0])
	{
		UseDefaultRollBinding();
		fprintf(stderr,
			"\e[1;95m[LNCT]\e[0m WARN: BG3 input config unavailable; using middle mouse button\n");
	}
	return 0;
}

static void MaybeReloadRollBindings(void)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;
	long long elapsed_ns = (long long)(now.tv_sec - g_last_binding_check.tv_sec) * 1000000000LL
		+ (now.tv_nsec - g_last_binding_check.tv_nsec);
	if (elapsed_ns >= 0 && elapsed_ns < BINDING_RELOAD_INTERVAL_NS)
		return;
	g_last_binding_check = now;

	LNCT_FileStamp current_stamp;
	if (!LNCT_ReadFileStamp(g_input_config_path, &current_stamp))
	{
		ReloadRollBindings();
		return;
	}
	if (LNCT_FileStampEqual(&current_stamp, &g_input_config_stamp))
	{
		return;
	}

	/* Reload through a temporary BindingSet. A partially written file leaves
	 * the last known-good binding active and is retried on the next poll. */
	if (ReloadRollBindings())
		ResetInputState();
}


static long long ElapsedNs(const struct timespec* from, const struct timespec* to)
{
	return (long long)(to->tv_sec - from->tv_sec) * 1000000000LL
		+ (to->tv_nsec - from->tv_nsec);
}

/* Find this camera's entry, recycling the least recently updated one if new. */
static CameraTiming* GetCameraEntry(void* camera_object, const struct timespec* now,
	float* out_delta_time)
{
	*out_delta_time = 0.f;
	CameraTiming* oldest = &g_camera_timings[0];
	for (int i = 0; i < MAX_TRACKED_CAMERAS; i++)
	{
		CameraTiming* timing = &g_camera_timings[i];
		if (timing->camera_object == camera_object)
		{
			float delta_time = (float)ElapsedNs(&timing->last_update, now) / 1000000000.f;
			timing->last_update = *now;
			if (delta_time < 0.f)
				delta_time = 0.f;
			if (delta_time > MAX_CAMERA_DELTA_TIME)
				delta_time = MAX_CAMERA_DELTA_TIME;
			*out_delta_time = delta_time;
			return timing;
		}
		if (!timing->camera_object)
			oldest = timing; /* a free slot always wins */
		else if (oldest->camera_object
			&& ElapsedNs(&timing->last_update, &oldest->last_update) > 0)
			oldest = timing;
	}

	/* A new camera gets a timing baseline; movement starts on its next update. */
	memset(oldest, 0, sizeof(*oldest));
	oldest->camera_object = camera_object;
	oldest->last_update = *now;
	return oldest;
}

static int CameraIsActive(const CameraTiming* timing, const struct timespec* now)
{
	return timing->camera_object
		&& ElapsedNs(&timing->last_update, now) < CAMERA_ACTIVE_WINDOW_NS;
}

static int CountActiveCameras(const struct timespec* now)
{
	int count = 0;
	for (int i = 0; i < MAX_TRACKED_CAMERAS; i++)
		if (CameraIsActive(&g_camera_timings[i], now))
			count++;
	return count;
}

/*
 * Splitscreen player index of a camera: 0 or 1 if it is one half of an active
 * pair of viewport cameras (two cameras exactly one stride apart), else -1.
 * The lower address is player 1's viewport.
 */
static int SplitscreenPlayerForCamera(const CameraTiming* entry, const struct timespec* now)
{
	uintptr_t self = (uintptr_t)entry->camera_object;
	for (int i = 0; i < MAX_TRACKED_CAMERAS; i++)
	{
		const CameraTiming* other = &g_camera_timings[i];
		if (other == entry || !CameraIsActive(other, now))
			continue;
		uintptr_t addr = (uintptr_t)other->camera_object;
		if (addr == self + SPLITSCREEN_CAMERA_STRIDE)
			return 0;
		if (addr + SPLITSCREEN_CAMERA_STRIDE == self)
			return 1;
	}
	return -1;
}

/*
 * Controller for splitscreen player 0/1: connected controllers in SDL
 * connection order (lowest instance id first). swap_splitscreen_controllers
 * reverses the two if BG3 assigned them the other way round.
 */
static int PadForPlayer(int player)
{
	int order[MAX_TRACKED_PADS];
	int count = 0;
	for (int i = 0; i < MAX_TRACKED_PADS; i++)
	{
		if (!PadValid(i))
			continue;
		int id = atomic_load(&g_pads[i].instance_id);
		int pos = count++;
		while (pos > 0 && atomic_load(&g_pads[order[pos - 1]].instance_id) > id)
		{
			order[pos] = order[pos - 1];
			pos--;
		}
		order[pos] = i;
	}
	if (count >= 2 && g_config.swap_splitscreen_controllers && player < 2)
		player = 1 - player;
	return player < count ? order[player] : -1;
}

/*
 * Splitscreen player tracking.
 *
 * BG3 does not keep a player's camera in one object: it swaps the contents of
 * the two viewport cameras and sometimes moves a player's camera into a new
 * object elsewhere in memory. So a player is followed by the camera's
 * contents instead: where it looks (root position), its pitch and its zoom.
 * These only change a little between frames, while the two players' cameras
 * normally differ a lot.
 *
 * Tracks are seeded from the adjacent viewport pair when splitscreen starts
 * (lower address = player 1); after that, continuity decides.
 */
/*
 * Player tag stored in the lowest 6 mantissa bits of the pitch float.
 * BG3 never writes pitch itself (the mod NOPs that store) and copies it
 * verbatim when it swaps or moves camera objects, so the tag travels with the
 * player's camera. The change is below 0.0003 degrees.
 */
#define PITCH_TAG_MASK 0x3Fu
static const uint32_t k_pitch_tags[2] = { 0x15u, 0x2Au };

static int PitchTagPlayer(const void* camera_object)
{
	uint32_t bits;
	memcpy(&bits, (const uint8_t*)camera_object + 0x164, sizeof(bits));
	if ((bits & PITCH_TAG_MASK) == k_pitch_tags[0])
		return 0;
	if ((bits & PITCH_TAG_MASK) == k_pitch_tags[1])
		return 1;
	return -1;
}

static float TagPitch(float pitch, int player)
{
	uint32_t bits;
	memcpy(&bits, &pitch, sizeof(bits));
	bits = (bits & ~PITCH_TAG_MASK) | k_pitch_tags[player];
	memcpy(&pitch, &bits, sizeof(pitch));
	return pitch;
}

/* Pitch written for no particular player must never look like a tag by chance. */
static float UntagPitch(float pitch)
{
	uint32_t bits;
	memcpy(&bits, &pitch, sizeof(bits));
	uint32_t low = bits & PITCH_TAG_MASK;
	if (low == k_pitch_tags[0] || low == k_pitch_tags[1])
		bits &= ~PITCH_TAG_MASK;
	memcpy(&pitch, &bits, sizeof(pitch));
	return pitch;
}

static void StampPitchTag(void* camera_object, int player)
{
	float* pitch = (float*)((uint8_t*)camera_object + 0x164);
	if (isfinite(*pitch) && PitchTagPlayer(camera_object) != player)
		*pitch = TagPitch(*pitch, player);
}

#define TRACK_STATE_SIZE 5
#define TRACK_STALE_NS 1000000000L      /* track not seen for 1 s: splitscreen over */
#define TRACK_REACQUIRE_NS 100000000L   /* track not seen for 100 ms: may be re-acquired */
#define TRACK_MATCH_MAX 5.0f
#define TRACK_TIE 0.05f

typedef struct
{
	int valid;
	void* camera_object;
	float state[TRACK_STATE_SIZE];
	struct timespec updated;
} PlayerTrack;
static PlayerTrack g_tracks[2];

static int ReadCameraState(const void* camera_object, float* state)
{
	const uint8_t* cam = (const uint8_t*)camera_object;
	state[0] = *(const float*)(cam + LNCT_CAMERA_ROOT_POS_OFFSET);
	state[1] = *(const float*)(cam + LNCT_CAMERA_ROOT_POS_OFFSET + 4);
	state[2] = *(const float*)(cam + LNCT_CAMERA_ROOT_POS_OFFSET + 8);
	state[3] = *(const float*)(cam + 0x164);
	state[4] = *(const float*)(cam + LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET);
	for (int i = 0; i < TRACK_STATE_SIZE; i++)
		if (!isfinite(state[i]) || fabsf(state[i]) > 1e6f)
			return 0;
	/* Short-lived helper objects carry zero zoom/pitch: not a player camera. */
	return state[4] >= 0.5f && state[3] != 0.f;
}

static float CameraStateDistance(const float* a, const float* b)
{
	float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return sqrtf(dx * dx + dy * dy + dz * dz)
		+ 0.1f * fabsf(a[3] - b[3])
		+ 0.5f * fabsf(a[4] - b[4]);
}

static int TrackFresh(const PlayerTrack* t, const struct timespec* now, long long max_age)
{
	return t->valid && ElapsedNs(&t->updated, now) < max_age;
}

static int SplitscreenActive(const struct timespec* now)
{
	return TrackFresh(&g_tracks[0], now, TRACK_STALE_NS)
		&& TrackFresh(&g_tracks[1], now, TRACK_STALE_NS);
}

static void UpdateTrack(int player, CameraTiming* entry, const float* state,
	const struct timespec* now, const char* why)
{
	PlayerTrack* t = &g_tracks[player];
	static struct timespec last_log[2];
	if (LNCT_DbgOn() && t->camera_object != entry->camera_object
		&& ElapsedNs(&last_log[player], now) > 250000000LL)
	{
		last_log[player] = *now;
		fprintf(stderr, "[LNCT-DBG] player %d camera now %p (%s) root=(%.2f %.2f %.2f) pitch=%.2f zoom=%.2f\n",
			player + 1, entry->camera_object, why,
			state[0], state[1], state[2], state[3], state[4]);
	}
	t->valid = 1;
	t->camera_object = entry->camera_object;
	memcpy(t->state, state, sizeof(t->state));
	t->updated = *now;
}

/* Active camera exactly one stride away from this one, or NULL. */
static const CameraTiming* PairPartner(const CameraTiming* entry, const struct timespec* now)
{
	uintptr_t self = (uintptr_t)entry->camera_object;
	for (int i = 0; i < MAX_TRACKED_CAMERAS; i++)
	{
		const CameraTiming* other = &g_camera_timings[i];
		if (other == entry || !CameraIsActive(other, now))
			continue;
		uintptr_t addr = (uintptr_t)other->camera_object;
		if (addr == self + SPLITSCREEN_CAMERA_STRIDE || addr + SPLITSCREEN_CAMERA_STRIDE == self)
			return other;
	}
	return NULL;
}

/*
 * Splitscreen player (0/1) currently shown by this camera, or -1.
 * Order of evidence: the pitch tag (survives swaps, moves and combat), then
 * the adjacent pair (only to start splitscreen), then camera continuity.
 * Every identified camera gets the tag stamped so later copies carry it.
 */
static int IdentifyPlayer(CameraTiming* entry, const float* state, const struct timespec* now,
	const char** why)
{
	int tagged = PitchTagPlayer(entry->camera_object);
	if (tagged >= 0)
	{
		*why = "pitch tag";
		return tagged;
	}

	if (!SplitscreenActive(now))
	{
		const CameraTiming* partner = PairPartner(entry, now);
		if (!partner)
			return -1;
		int partner_tag = PitchTagPlayer(partner->camera_object);
		*why = "splitscreen start";
		if (partner_tag >= 0)
			return 1 - partner_tag;
		return SplitscreenPlayerForCamera(entry, now);
	}

	float d0 = CameraStateDistance(state, g_tracks[0].state);
	float d1 = CameraStateDistance(state, g_tracks[1].state);
	int player = d0 <= d1 ? 0 : 1;
	if (fabsf(d0 - d1) < TRACK_TIE && g_tracks[1 - player].camera_object == entry->camera_object)
		player = 1 - player;
	*why = "followed";
	if ((player == 0 ? d0 : d1) > TRACK_MATCH_MAX)
	{
		int stale0 = !TrackFresh(&g_tracks[0], now, TRACK_REACQUIRE_NS);
		int stale1 = !TrackFresh(&g_tracks[1], now, TRACK_REACQUIRE_NS);
		if (stale0 == stale1)
			return -1;
		player = stale0 ? 0 : 1;
		*why = "re-acquired";
	}
	return player;
}

static int PlayerForCamera(CameraTiming* entry, const struct timespec* now)
{
	float state[TRACK_STATE_SIZE];
	if (!ReadCameraState(entry->camera_object, state))
		return -1;

	const char* why = "";
	int player = IdentifyPlayer(entry, state, now, &why);
	if (player < 0)
		return -1;

	StampPitchTag(entry->camera_object, player);
	UpdateTrack(player, entry, state, now, why);
	/*
	 * A tagged camera stays with its player even when it is the only one the
	 * hook sees (BG3's combat view for the other player bypasses this hook).
	 */
	return player;
}


/* Which controller's stick/L3 should drive this camera right now (-1 = none). */
static int SelectPadForCamera(CameraTiming* entry, const struct timespec* now, int* out_player,
	int vanilla)
{
	long long now_ns = (long long)now->tv_sec * 1000000000LL + now->tv_nsec;
	int player = PlayerForCamera(entry, now);
	*out_player = player;
	if (SplitscreenActive(now) && !g_splitscreen_seen)
	{
		g_splitscreen_seen = 1;
		UpdateVanillaCombatEnabled();
	}
	if (player >= 0)
	{
		int pad = PadForPlayer(player);
		/* A pad whose camera is in vanilla combat gets its input passed to BG3. */
		if (pad >= 0 && !vanilla)
			atomic_store(&g_pads[pad].camera_owned_ns, now_ns);
		return pad;
	}
	if (SplitscreenActive(now))
		return -1; /* helper camera during splitscreen */

	/* One untagged viewport: behave exactly like the single-player mod. */
	for (int i = 0; i < MAX_TRACKED_PADS && !vanilla; i++)
		if (PadValid(i))
			atomic_store(&g_pads[i].camera_owned_ns, now_ns);
	int last = atomic_load(&g_last_active_pad);
	return PadValid(last) ? last : -1;
}


float H_CalculateCameraAngle_CallSite(void* pCameraObject, uint8_t angle)
{
	if (!g_setup_succeeded)
		return O_CalculateCameraAngle(pCameraObject, angle);

	LNCT_DbgCameraCall(pCameraObject);
	RefreshControllerState();

	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return O_CalculateCameraAngle(pCameraObject, angle);
	float delta_time;
	CameraTiming* entry = GetCameraEntry(pCameraObject, &now, &delta_time);
	int camera_player = -1;
	int vanilla_combat = CameraInVanillaCombat(pCameraObject);
	int pad_slot = SelectPadForCamera(entry, &now, &camera_player, vanilla_combat);

	float* pitch_field = (float*)((uint8_t*)pCameraObject + 0x164);
	if (vanilla_combat)
	{
		float game = O_CalculateCameraAngle(pCameraObject, angle); /* BG3's own controls */
		LNCT_DbgCameraValues(pCameraObject, game, *pitch_field);
		return game;
	}
	PadState* pad = pad_slot >= 0 ? &g_pads[pad_slot] : NULL;

	int roll_keydown = atomic_load(&g_roll_keydown);
	int left_stick_button_down = pad ? atomic_load(&pad->left_stick_button_down) : 0;
	int right_stick_axis_motion_y = pad ? atomic_load(&pad->right_stick_y_active) : 0;
	int16_t right_stick_y = pad ? (int16_t)atomic_load(&pad->right_stick_y) : 0;
	int zooming = !roll_keydown && left_stick_button_down && right_stick_axis_motion_y;
	if (zooming)
	{
		LNCT_ApplyZoomDelta(pCameraObject,
			LNCT_ControllerZoomDelta(right_stick_y, delta_time, g_config.controller_zoom_speed)
				* (g_config.invert_controller_zoom ? -1.f : 1.f));
		atomic_store(&pad->left_stick_used_for_zoom, 1);
	}
	else
	{
		int zoom = atomic_exchange(&g_mouse_wheel_y, 0);
		/* A zero delta must not overwrite BG3's in-progress zoom interpolation. */
		if (zoom != 0)
			LNCT_ApplyZoomDelta(pCameraObject, -((float)zoom * MOUSE_ZOOM_FACTOR));
	}

	int pitch_input = !zooming && (roll_keydown || right_stick_axis_motion_y);
	if (pitch_input)
	{
		g_roll = pitch_field;
		float roll = *pitch_field;

		/* An explicitly held mouse-rotate binding wins over stale controller state. */
		if (right_stick_axis_motion_y && !roll_keydown)
		{
			/*
			 * The camera's 0xC4 value is not a rotation speed on Linux (it
			 * tracks the camera's x position), so by default pitch uses a
			 * fixed reference speed: same stick, same speed everywhere.
			 */
			float rotation_speed = g_config.controller_pitch_follows_game_speed
				? LNCT_StableRotationSpeed(
					*(float*)((uint8_t*)pCameraObject + CAMERA_OBJECT_ROTATION_SPEED_OFFSET))
				: LNCT_REFERENCE_ROTATION_SPEED;
			float pitch_delta = LNCT_ControllerPitchDelta(right_stick_y, delta_time, rotation_speed)
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
		if (roll < -89.f)
			roll = -89.f;
		*pitch_field = camera_player >= 0 ? TagPitch(roll, camera_player) : UntagPitch(roll);
	}

	float game = O_CalculateCameraAngle(pCameraObject, angle);
	LNCT_DbgCameraValues(pCameraObject, game, *pitch_field);
	return game;
}

static int IsExpectedPitchStore(const uint8_t* instruction)
{
	return instruction[0] == 0xF3 && instruction[1] == 0x0F
		&& instruction[2] == 0x11 && (instruction[3] & 0xC7) == 0x85
		&& instruction[4] == 0x64 && instruction[5] == 0x01
		&& instruction[6] == 0x00 && instruction[7] == 0x00;
}

/*
 * Builds the stub that replaces BG3's pitch store `movss [rbp+0x164], xmmK`.
 *   - Vanilla combat enabled and this camera in combat: perform BG3's store,
 *     keeping the low 6 bits of the old value so the player tag survives.
 *   - Otherwise, sync method: skip the store and load the mod's pitch from
 *     0x164 into xmmK instead (`movss xmmK, [rbp+0x164]`), so BG3's code after
 *     this point uses the same pitch that is stored. Leaving BG3's own value in
 *     the register is what made the camera micro-jitter.
 *   - Otherwise (direct method): do nothing, as the original mod did.
 * Flags, rax and rcx are preserved; the red zone below rsp is skipped.
 * Returns the stub length.
 */
static size_t BuildPitchStoreStub(uint8_t* stub, const uint8_t* movss, const volatile uint8_t* enable)
{
	uint8_t xmm = (movss[3] >> 3) & 7;
	size_t o = 0;
	const uint8_t prologue[] = {
		0x48, 0x8D, 0x64, 0x24, 0x80, /* lea rsp,[rsp-0x80] */
		0x9C,                         /* pushfq */
		0x50,                         /* push rax */
		0x51,                         /* push rcx */
	};
	memcpy(stub + o, prologue, sizeof(prologue)); o += sizeof(prologue);

	/* test byte [rip+enable],1 ; jz not_combat */
	stub[o++] = 0xF6; stub[o++] = 0x05;
	size_t rel_at = o; o += 4;
	stub[o++] = 0x01;
	int32_t rel = (int32_t)((const uint8_t*)enable - (stub + o));
	memcpy(stub + rel_at, &rel, 4);
	stub[o++] = 0x74; size_t jz_nc1 = o++;

	const uint8_t combat_test[] = { 0xF6, 0x85, 0xA8, 0x00, 0x00, 0x00, 0x01 }; /* test byte [rbp+0xA8],1 */
	memcpy(stub + o, combat_test, sizeof(combat_test)); o += sizeof(combat_test);
	stub[o++] = 0x74; size_t jz_nc2 = o++;        /* jz not_combat */

	const uint8_t store[] = {
		0x66, 0x0F, 0x7E, (uint8_t)(0xC0 | (xmm << 3)), /* movd eax,xmmK */
		0x8B, 0x8D, 0x64, 0x01, 0x00, 0x00,             /* mov ecx,[rbp+0x164] */
		0x83, 0xE1, 0x3F,                               /* and ecx,0x3F */
		0x25, 0xC0, 0xFF, 0xFF, 0xFF,                   /* and eax,0xFFFFFFC0 */
		0x09, 0xC8,                                     /* or eax,ecx */
		0x89, 0x85, 0x64, 0x01, 0x00, 0x00,             /* mov [rbp+0x164],eax */
	};
	memcpy(stub + o, store, sizeof(store)); o += sizeof(store);
	stub[o++] = 0xEB; size_t jmp_done = o++;       /* jmp done */

	size_t not_combat = o;
	stub[jz_nc1] = (uint8_t)(not_combat - (jz_nc1 + 1));
	stub[jz_nc2] = (uint8_t)(not_combat - (jz_nc2 + 1));

	/* test byte [rip+enable],2 ; jz done */
	stub[o++] = 0xF6; stub[o++] = 0x05;
	rel_at = o; o += 4;
	stub[o++] = 0x02;
	rel = (int32_t)((const uint8_t*)enable - (stub + o));
	memcpy(stub + rel_at, &rel, 4);
	stub[o++] = 0x74; size_t jz_done = o++;

	/* movss xmmK,[rbp+0x164]: BG3's own instruction with load instead of store */
	const uint8_t load[8] = { 0xF3, 0x0F, 0x10, movss[3], movss[4], movss[5], movss[6], movss[7] };
	memcpy(stub + o, load, sizeof(load)); o += sizeof(load);

	size_t done = o;
	stub[jmp_done] = (uint8_t)(done - (jmp_done + 1));
	stub[jz_done] = (uint8_t)(done - (jz_done + 1));
	const uint8_t epilogue[] = {
		0x59,                                           /* pop rcx */
		0x58,                                           /* pop rax */
		0x9D,                                           /* popfq */
		0x48, 0x8D, 0xA4, 0x24, 0x80, 0x00, 0x00, 0x00, /* lea rsp,[rsp+0x80] */
	};
	memcpy(stub + o, epilogue, sizeof(epilogue)); o += sizeof(epilogue);

	stub[o++] = 0xE9;                              /* jmp movss+8 */
	int32_t back = (int32_t)((movss + 8) - (stub + o + 4));
	memcpy(stub + o, &back, 4); o += 4;
	return o;
}

/* Overwrites the 8-byte store with `jmp stub` + 3 NOPs. */
static void BuildPitchStoreJump(uint8_t out[8], const uint8_t* movss, const uint8_t* stub)
{
	int32_t rel = (int32_t)(stub - (movss + 5));
	out[0] = 0xE9;
	memcpy(out + 1, &rel, 4);
	out[5] = out[6] = out[7] = 0x90;
}

#define PITCH_STUB_ENABLE_OFFSET 0x200

uint8_t PatchUpdateCamera()
{
	uint8_t* movss = (uint8_t*)GetAddresses()->roll_movss;
	if (!IsExpectedPitchStore(movss))
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Refusing unexpected pitch patch instruction\n");
		return 0;
	}
	long page_size = sysconf(_SC_PAGESIZE);

	uint8_t replacement[8] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 };
	uint8_t* stub = (uint8_t*)AllocNear(movss, (size_t)page_size);
	if (stub)
	{
		volatile uint8_t* enable = stub + PITCH_STUB_ENABLE_OFFSET;
		*enable = 0;
		BuildPitchStoreStub(stub, movss, enable);
		BuildPitchStoreJump(replacement, movss, stub);
		g_vanilla_combat_enabled = enable;
		UpdateVanillaCombatEnabled();
	}
	else
	{
		/* Fall back to the old behaviour: no vanilla pitch in combat. */
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: no stub memory; combat pitch stays modded\n");
	}

	uint64_t page_start = (uint64_t)movss & ~(page_size - 1);
	size_t num_pages = (((uint64_t)movss + 8 - page_start) + page_size - 1) / page_size;
	if (num_pages < 1)
		num_pages = 1;

	if (mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
	{
		fprintf(stdout, "\e[1;95m[LNCT]\e[0m ERR: PatchUpdateCamera(): roll movss mprotect() failed\n");
		return 0;
	}

	uint8_t original[8];
	memcpy(original, movss, sizeof(original));
	memcpy(movss, replacement, sizeof(replacement));
	if (mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_EXEC) != 0)
	{
		memcpy(movss, original, sizeof(original));
		mprotect((void*)page_start, num_pages * page_size, PROT_READ | PROT_EXEC);
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: PatchUpdateCamera(): couldn't restore executable page protection\n");
		return 0;
	}

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
	InitPads();

	fprintf(stdout, "\e[1;95m[LNCT]\e[0m \e[1;37mLinux Native Camera Tweaks %s\e[0m\n", VERSION);
	fprintf(stdout, "\e[1;95m[LNCT]\e[0m Bug(s) ? Suggestion(s) ? Add me on discord: biiinks78\n");
	g_game_build = GetBuild();
	if (g_game_build)
		fprintf(stdout, "\e[1;95m[LNCT]\e[0m Known BG3 build detected: %lu\n", g_game_build);
	else
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: Unknown BG3 build; validating patterns and patch instructions\n");
	if (LNCT_LoadConfig(&g_config, g_config_path, sizeof(g_config_path)))
	{
		fprintf(stdout,
			"\e[1;95m[LNCT]\e[0m Config: %s (controller pitch %.3f, controller zoom %.3f, mouse pitch %.3f, pitch inverted %s, zoom inverted %s)\n",
			g_config_path,
			g_config.controller_pitch_sensitivity,
			g_config.controller_zoom_speed,
			g_config.mouse_pitch_sensitivity,
			g_config.invert_controller_pitch ? "yes" : "no",
			g_config.invert_controller_zoom ? "yes" : "no");
	}
	else
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m WARN: Couldn't load or create controller config; using defaults\n");
	}

	ReloadRollBindings();

	struct Sigs* sigs = GetSigs();
	struct Addresses* addresses = GetAddresses();
	uint64_t camera_pattern = PatternScanSectionUnique(sigs->CalculateCameraAngle_Callsite, ".text");
	if (!camera_pattern)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): camera pattern missing or ambiguous\n");
		return;
	}
	addresses->CalculateCameraAngle_CallSite = camera_pattern + 39;
	addresses->roll_movss = PatternScanSectionUnique(sigs->roll_movss, ".text");
	if (!addresses->roll_movss)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): pitch pattern missing or ambiguous\n");
		return;
	}

	O_CalculateCameraAngle = ResolveCallTarget((void*)(GetAddresses()->CalculateCameraAngle_CallSite));
	if (!O_CalculateCameraAngle)
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Couldn't resolve the original BG3 camera function\n");
		return;
	}

	/* Install the guarded call-site first. Until setup succeeds it is a pure
	 * pass-through, so a later pitch-patch failure leaves BG3 behavior intact. */
	if (!SetupCallSitesTrampoline())
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): SetupCallSitesTrampoline() failed\n");
		return;
	}

	if (!PatchUpdateCamera())
	{
		fprintf(stderr, "\e[1;95m[LNCT]\e[0m ERR: Setup(): PatchUpdateCamera() failed\n");
		return;
	}

	fprintf(stdout, "\e[1;95m[LNCT]\e[0m Pitch method: %s\n",
		g_config.pitch_method == LNCT_PITCH_SYNC ? "sync" : "direct");
	fprintf(stdout, "\e[1;95m[LNCT]\e[0m Vanilla combat camera: %s\n",
		g_config.vanilla_combat_camera == LNCT_VANILLA_COMBAT_ALWAYS ? "always"
		: g_config.vanilla_combat_camera == LNCT_VANILLA_COMBAT_NEVER ? "never" : "after splitscreen");
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
/* Did any camera pick this pad recently? Otherwise its input stays vanilla. */
static int PadHasCamera(int slot)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 1;
	long long now_ns = (long long)now.tv_sec * 1000000000LL + now.tv_nsec;
	long long owned = atomic_load(&g_pads[slot].camera_owned_ns);
	return owned != 0 && now_ns - owned < PAD_CAMERA_GRACE_NS;
}

static int HandleSDLEvent(const SDL_Event* event)
{
	LNCT_DbgEvent(event);
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
	/* Register every controller as soon as it does anything, so splitscreen
	 * player order (by SDL instance id) is known before it touches the camera. */
	if (event->type == SDL_CONTROLLERAXISMOTION)
		PadSlotForInstance(event->caxis.which, 1);
	else if (event->type == SDL_CONTROLLERBUTTONDOWN || event->type == SDL_CONTROLLERBUTTONUP)
		PadSlotForInstance(event->cbutton.which, 1);

	if (event->type == SDL_CONTROLLERAXISMOTION
		&& event->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY)
	{
		int slot = PadSlotForInstance(event->caxis.which, 0);
		if (slot >= 0 && !PadHasCamera(slot))
		{
			atomic_store(&g_pads[slot].right_stick_y, 0);
			atomic_store(&g_pads[slot].right_stick_y_active, 0);
			return 0; /* vanilla: BG3 handles this pad's stick */
		}
	}
	if (event->type == SDL_CONTROLLERAXISMOTION
		&& (event->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY
			|| event->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX))
	{
		int slot = PadSlotForInstance(event->caxis.which, 1);
		if (slot < 0)
			return 0;
		PadState* pad = &g_pads[slot];
		int16_t value = event->caxis.value;
		int axis_active = LNCT_NormalizeControllerAxis(value) != 0.f;
		if (event->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX)
		{
			/* Observed for camera ownership only; BG3 still handles it. */
			atomic_store(&pad->right_stick_x, value);
			return 0;
		}
		if (axis_active)
			atomic_store(&g_last_active_pad, slot);
		atomic_store(&pad->right_stick_y, value);
		atomic_store(&pad->right_stick_y_active, axis_active);
		if (axis_active && atomic_load(&pad->left_stick_button_down))
			atomic_store(&pad->left_stick_used_for_zoom, 1);
		return 1;
	}
	else if ((event->type == SDL_CONTROLLERBUTTONDOWN) && event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK)
	{
		int slot = PadSlotForInstance(event->cbutton.which, 1);
		if (slot < 0)
			return 0;
		PadState* pad = &g_pads[slot];
		if (!PadHasCamera(slot))
		{
			pad->l3_passthrough = 1;
			return 0; /* vanilla L3 for a pad without a modded camera */
		}
		pad->l3_passthrough = 0;
		atomic_store(&pad->l3_held_by_mod, 1);
		atomic_store(&g_last_active_pad, slot);
		atomic_store(&pad->left_stick_button_down, 1);
		atomic_store(&pad->left_stick_used_for_zoom,
			atomic_load(&pad->right_stick_y_active));
		pad->left_stick_down_event = *event;
		pad->has_left_stick_down_event = 1;
		return 1;
	}
	else if ((event->type == SDL_CONTROLLERBUTTONUP) && event->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK)
	{
		int slot = PadSlotForInstance(event->cbutton.which, 1);
		if (slot < 0)
			return 0;
		PadState* pad = &g_pads[slot];
		if (pad->l3_passthrough)
		{
			pad->l3_passthrough = 0;
			return 0;
		}
		int used_for_zoom = atomic_exchange(&pad->left_stick_used_for_zoom, 0);
		atomic_store(&pad->l3_held_by_mod, 0);
		atomic_store(&pad->left_stick_button_down, 0);
		if (!used_for_zoom && pad->has_left_stick_down_event
			&& g_deferred_controller_event_count < DEFERRED_EVENT_CAPACITY
			&& g_next_frame_event_count < DEFERRED_EVENT_CAPACITY)
		{
			long long release_ns = MonotonicNs() + L3_REPLAY_HOLD_NS;
			atomic_store(&pad->l3_replay_until_ns, release_ns);
			g_deferred_controller_events[g_deferred_controller_event_count++] =
				pad->left_stick_down_event;
			g_next_frame_release_ns[g_next_frame_event_count] = release_ns;
			g_next_frame_events[g_next_frame_event_count++] = *event;
		}
		pad->has_left_stick_down_event = 0;
		return 1;
	}
	else if (event->type == SDL_CONTROLLERDEVICEREMOVED)
	{
		RemovePad(event->cdevice.which);
	}
	else if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
	{
		ResetControllerState();
		ResetInputState();
		g_deferred_controller_event_count = 0;
		g_deferred_controller_event_index = 0;
		g_next_frame_event_count = 0;
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
	MaybeReloadRollBindings();
	LNCT_DbgMaybePrint(atomic_load(&g_last_active_pad), g_setup_succeeded);

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

	/* Queue drained: this frame's input is done. Due releases go out next frame. */
	PromoteNextFrameEvents(MonotonicNs());
	return ret;
}


/*
 * BG3 may also poll L3 directly. Keep the polled state consistent with the
 * events the mod delivers: released while the mod is still deciding between
 * click and zoom, pressed for the length of a replayed click.
 */
static atomic_int g_dbg_logged_poll_l3;

Uint8 SDL_GameControllerGetButton(SDL_GameController* controller, SDL_GameControllerButton button)
{
	if (!O_SDL_GameControllerGetButton)
		ResolveSDLSym();
	if (!O_SDL_GameControllerGetButton)
		return 0;
	Uint8 physical = O_SDL_GameControllerGetButton(controller, button);
	if (button != SDL_CONTROLLER_BUTTON_LEFTSTICK || !g_setup_succeeded
		|| !O_SDL_GameControllerFromInstanceID)
		return physical;

	if (LNCT_DbgOn() && !atomic_exchange(&g_dbg_logged_poll_l3, 1))
		fprintf(stderr, "[LNCT-DBG] BG3 polls L3 via SDL_GameControllerGetButton\n");

	for (int i = 0; i < MAX_TRACKED_PADS; i++)
	{
		int id = atomic_load(&g_pads[i].instance_id);
		if (id < 0 || O_SDL_GameControllerFromInstanceID(id) != controller)
			continue;
		if (MonotonicNs() < atomic_load(&g_pads[i].l3_replay_until_ns))
			return 1;
		if (atomic_load(&g_pads[i].l3_held_by_mod))
			return 0;
		return physical;
	}
	return physical;
}

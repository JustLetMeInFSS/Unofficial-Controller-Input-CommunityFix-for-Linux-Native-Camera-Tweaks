#pragma once

/*
 * Splitscreen diagnostics. Enabled only when the environment variable
 * LNCT_DEBUG=1 is set. Prints a one-second summary to stderr:
 *   - every camera object the hook saw, with call counts, pitch and zoom
 *   - every controller instance that sent events, with right-stick Y and L3
 *   - which controller the mod is currently reading live state from
 * Counters are best-effort (racy between threads); good enough for diagnosis.
 */

#include <SDL2/SDL.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define LNCT_DBG_SLOTS 32

typedef struct
{
	void* camera;
	atomic_uint calls;
	unsigned total_calls;
	unsigned idle_prints;
	/* Per-second range of BG3's computed pitch (hook return) and of 0x164. */
	float game_min, game_max, pitch_min, pitch_max;
	int have_values;
} LNCT_DbgCamera;

typedef struct
{
	int used;
	SDL_JoystickID id;
	unsigned righty_events;
	unsigned rightx_events;
	unsigned l3_events;
	int last_righty;
	int last_rightx;
} LNCT_DbgPad;

static int g_dbg_enabled = -1;
static LNCT_DbgCamera g_dbg_cameras[LNCT_DBG_SLOTS];
static LNCT_DbgPad g_dbg_pads[LNCT_DBG_SLOTS];
static struct timespec g_dbg_last_print;
static atomic_uint g_dbg_hook_calls_total;
static atomic_int g_dbg_refresh_id = -1;
static atomic_int g_dbg_refresh_righty;
static atomic_int g_dbg_refresh_l3;

static inline int LNCT_DbgOn(void)
{
	if (g_dbg_enabled < 0)
	{
		const char* v = getenv("LNCT_DEBUG");
		g_dbg_enabled = (v && v[0] == '1') ? 1 : 0;
		if (g_dbg_enabled)
			fprintf(stderr, "[LNCT-DBG] splitscreen diagnostics enabled\n");
	}
	return g_dbg_enabled;
}

/* Called from the camera hook (game thread). */
static inline void LNCT_DbgCameraCall(void* camera)
{
	if (!LNCT_DbgOn())
		return;
	atomic_fetch_add(&g_dbg_hook_calls_total, 1);
	for (int i = 0; i < LNCT_DBG_SLOTS; i++)
	{
		if (g_dbg_cameras[i].camera == camera)
		{
			atomic_fetch_add(&g_dbg_cameras[i].calls, 1);
			return;
		}
	}
	for (int i = 0; i < LNCT_DBG_SLOTS; i++)
	{
		if (!g_dbg_cameras[i].camera)
		{
			g_dbg_cameras[i].camera = camera;
			atomic_store(&g_dbg_cameras[i].calls, 1);
			fprintf(stderr, "[LNCT-DBG] new camera object #%d at %p\n", i, camera);
			return;
		}
	}
}

/* Called from the camera hook with BG3's computed pitch and the stored pitch. */
static inline void LNCT_DbgCameraValues(void* camera, float game, float pitch)
{
	if (!LNCT_DbgOn())
		return;
	for (int i = 0; i < LNCT_DBG_SLOTS; i++)
	{
		LNCT_DbgCamera* c = &g_dbg_cameras[i];
		if (c->camera != camera)
			continue;
		if (!c->have_values)
		{
			c->game_min = c->game_max = game;
			c->pitch_min = c->pitch_max = pitch;
			c->have_values = 1;
			return;
		}
		if (game < c->game_min) c->game_min = game;
		if (game > c->game_max) c->game_max = game;
		if (pitch < c->pitch_min) c->pitch_min = pitch;
		if (pitch > c->pitch_max) c->pitch_max = pitch;
		return;
	}
}

/* Called from RefreshControllerState with what it actually read. */
static inline void LNCT_DbgRefresh(int id, int righty, int l3)
{
	if (!LNCT_DbgOn())
		return;
	atomic_store(&g_dbg_refresh_id, id);
	atomic_store(&g_dbg_refresh_righty, righty);
	atomic_store(&g_dbg_refresh_l3, l3);
}

static inline LNCT_DbgPad* LNCT_DbgPadFor(SDL_JoystickID id)
{
	for (int i = 0; i < LNCT_DBG_SLOTS; i++)
		if (g_dbg_pads[i].used && g_dbg_pads[i].id == id)
			return &g_dbg_pads[i];
	for (int i = 0; i < LNCT_DBG_SLOTS; i++)
	{
		if (!g_dbg_pads[i].used)
		{
			g_dbg_pads[i].used = 1;
			g_dbg_pads[i].id = id;
			return &g_dbg_pads[i];
		}
	}
	return NULL;
}

/* Called for every SDL event BG3 polls (main thread). */
static inline void LNCT_DbgEvent(const SDL_Event* e)
{
	if (!LNCT_DbgOn())
		return;
	switch (e->type)
	{
	case SDL_CONTROLLERDEVICEADDED:
		fprintf(stderr, "[LNCT-DBG] controller ADDED device_index=%d\n", e->cdevice.which);
		break;
	case SDL_CONTROLLERDEVICEREMOVED:
		fprintf(stderr, "[LNCT-DBG] controller REMOVED instance=%d\n", e->cdevice.which);
		break;
	case SDL_CONTROLLERAXISMOTION:
	{
		LNCT_DbgPad* p = LNCT_DbgPadFor(e->caxis.which);
		if (!p)
			break;
		if (e->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTY)
		{
			p->righty_events++;
			p->last_righty = e->caxis.value;
		}
		else if (e->caxis.axis == SDL_CONTROLLER_AXIS_RIGHTX)
		{
			p->rightx_events++;
			p->last_rightx = e->caxis.value;
		}
		break;
	}
	case SDL_CONTROLLERBUTTONDOWN:
	case SDL_CONTROLLERBUTTONUP:
		if (e->cbutton.button == SDL_CONTROLLER_BUTTON_LEFTSTICK)
		{
			LNCT_DbgPad* p = LNCT_DbgPadFor(e->cbutton.which);
			if (p)
				p->l3_events++;
			fprintf(stderr, "[LNCT-DBG] L3 %s instance=%d\n",
				e->type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up", e->cbutton.which);
		}
		break;
	default:
		break;
	}
}

/* Called once per SDL_PollEvent; prints at most once per second. */
static inline void LNCT_DbgMaybePrint(int tracked_instance_id, int setup_ok)
{
	if (!LNCT_DbgOn())
		return;
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;
	if (now.tv_sec == g_dbg_last_print.tv_sec)
		return;
	g_dbg_last_print = now;

	fprintf(stderr, "[LNCT-DBG] ---- setup_ok=%d hook_calls_total=%u last_active_pad_slot=%d"
		" refresh(read pad=%d righty=%d l3=%d)\n",
		setup_ok, atomic_load(&g_dbg_hook_calls_total), tracked_instance_id,
		atomic_load(&g_dbg_refresh_id), atomic_load(&g_dbg_refresh_righty),
		atomic_load(&g_dbg_refresh_l3));

	for (int i = 0; i < LNCT_DBG_SLOTS; i++)
	{
		LNCT_DbgCamera* c = &g_dbg_cameras[i];
		if (!c->camera)
			continue;
		unsigned calls = atomic_exchange(&c->calls, 0);
		c->total_calls += calls;
		if (!calls)
		{
			/* Free slots of cameras that went away, so new ones get listed. */
			if (++c->idle_prints >= 3)
			{
				c->total_calls = 0;
				c->idle_prints = 0;
				c->camera = NULL;
			}
			continue; /* only show cameras updated in the last second */
		}
		c->idle_prints = 0;
		uint8_t* cam = (uint8_t*)c->camera;
		fprintf(stderr, "[LNCT-DBG]   camera #%d %p calls/s=%u total=%u pitch(0x164)=%.2f zoom(0x58)=%.2f root(0x18)=(%.1f %.1f %.1f) tag=%s flags=0x%x\n"
			"[LNCT-DBG]       game_pitch=[%.4f..%.4f] pitch_0x164=[%.4f..%.4f]\n",
			i, c->camera, calls, c->total_calls,
			*(float*)(cam + 0x164),
			*(float*)(cam + 0x58),
			*(float*)(cam + 0x18), *(float*)(cam + 0x1C), *(float*)(cam + 0x20),
			((*(uint32_t*)(cam + 0x164)) & 0x3Fu) == 0x15u ? "P1"
				: ((*(uint32_t*)(cam + 0x164)) & 0x3Fu) == 0x2Au ? "P2" : "-",
			*(uint32_t*)(cam + 0xA8),
			c->game_min, c->game_max, c->pitch_min, c->pitch_max);
		c->have_values = 0;
	}
	for (int i = 0; i < LNCT_DBG_SLOTS; i++)
	{
		LNCT_DbgPad* p = &g_dbg_pads[i];
		if (!p->used)
			continue;
		fprintf(stderr, "[LNCT-DBG]   pad instance=%d rightY ev/s=%u last=%d rightX ev/s=%u last=%d L3 ev/s=%u\n",
			p->id, p->righty_events, p->last_righty,
			p->rightx_events, p->last_rightx, p->l3_events);
		p->righty_events = 0;
		p->rightx_events = 0;
		p->l3_events = 0;
	}
}

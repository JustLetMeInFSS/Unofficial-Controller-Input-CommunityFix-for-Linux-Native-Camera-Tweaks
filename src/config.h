#pragma once

#include <stddef.h>


#define LNCT_DEFAULT_CONTROLLER_PITCH_SENSITIVITY 0.25f
#define LNCT_DEFAULT_CONTROLLER_ZOOM_SPEED 15.0f
#define LNCT_DEFAULT_MOUSE_PITCH_SENSITIVITY 1.50f
#define LNCT_DEFAULT_INVERT_CONTROLLER_ZOOM 0

/* Who gets BG3's own camera controls during combat. */
#define LNCT_VANILLA_COMBAT_NEVER 0
#define LNCT_VANILLA_COMBAT_SPLITSCREEN 1   /* default: only once splitscreen was used */
#define LNCT_VANILLA_COMBAT_ALWAYS 2

/* How the mod applies pitch. */
#define LNCT_PITCH_DIRECT 0  /* original: block BG3's pitch store only (jitters) */
#define LNCT_PITCH_SYNC 1    /* block the store and load the mod's pitch into BG3's register */

typedef struct
{
	float controller_pitch_sensitivity;
	float controller_zoom_speed;
	float mouse_pitch_sensitivity;
	int invert_controller_pitch;
	int invert_controller_zoom;
	int swap_splitscreen_controllers;
	int vanilla_combat_camera;
	int controller_pitch_follows_game_speed;
	int pitch_method;
} LNCT_Config;

void LNCT_SetConfigDefaults(LNCT_Config* config);
int LNCT_LoadConfig(LNCT_Config* config, char* path, size_t path_size);

#pragma once

#include <math.h>
#include <stdint.h>


#define LNCT_CAMERA_CURRENT_ZOOM_A_OFFSET 0x54
#define LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET 0x58
#define LNCT_CAMERA_DESIRED_ZOOM_OFFSET 0x5C

/*
 * BG3 interpolates currentZoomA/currentZoomB towards desiredZoom.  Moving only
 * one of them makes the game correct our value again on the following frame,
 * which presents as a permanent small zoom oscillation.
 */
static inline void LNCT_ApplyZoomDelta(void* camera_object, float delta)
{
	/* Leave BG3's own interpolation untouched when there is no custom input. */
	if (delta == 0.f || !isfinite(delta))
		return;

	uint8_t* camera = (uint8_t*)camera_object;
	float zoom = *(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET) + delta;
	*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_A_OFFSET) = zoom;
	*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET) = zoom;
	*(float*)(camera + LNCT_CAMERA_DESIRED_ZOOM_OFFSET) = zoom;
}

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

#include "camera_state.h"


static int NearlyEqual(float a, float b)
{
	return fabsf(a - b) < 0.0001f;
}

int main(void)
{
	uint8_t camera[0x60];
	memset(camera, 0, sizeof(camera));
	/* A pre-existing mismatch represents the persistent interpolation jitter. */
	*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_A_OFFSET) = 4.8f;
	*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET) = 5.f;
	*(float*)(camera + LNCT_CAMERA_DESIRED_ZOOM_OFFSET) = 5.4f;

	/* No custom input must not cancel BG3's active interpolation target. */
	LNCT_ApplyZoomDelta(camera, 0.f);
	assert(NearlyEqual(*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_A_OFFSET), 4.8f));
	assert(NearlyEqual(*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET), 5.f));
	assert(NearlyEqual(*(float*)(camera + LNCT_CAMERA_DESIRED_ZOOM_OFFSET), 5.4f));
	LNCT_ApplyZoomDelta(camera, NAN);
	assert(NearlyEqual(*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_A_OFFSET), 4.8f));
	assert(NearlyEqual(*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET), 5.f));
	assert(NearlyEqual(*(float*)(camera + LNCT_CAMERA_DESIRED_ZOOM_OFFSET), 5.4f));

	LNCT_ApplyZoomDelta(camera, -0.25f);

	assert(NearlyEqual(*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_A_OFFSET), 4.75f));
	assert(NearlyEqual(*(float*)(camera + LNCT_CAMERA_CURRENT_ZOOM_B_OFFSET), 4.75f));
	assert(NearlyEqual(*(float*)(camera + LNCT_CAMERA_DESIRED_ZOOM_OFFSET), 4.75f));
	return 0;
}

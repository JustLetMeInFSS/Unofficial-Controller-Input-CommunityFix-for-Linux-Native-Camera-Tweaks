#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"


int main(void)
{
	LNCT_Config config;
	char path[1024];
	assert(LNCT_LoadConfig(&config, path, sizeof(path)));
	assert(fabsf(config.controller_pitch_sensitivity - 0.25f) < 0.0001f);
	assert(fabsf(config.controller_zoom_speed - 15.0f) < 0.0001f);
	assert(fabsf(config.mouse_pitch_sensitivity - 1.50f) < 0.0001f);
	assert(config.invert_controller_pitch == 0);

	FILE* file = fopen(path, "w");
	assert(file);
	fprintf(file,
		"controller_pitch_sensitivity=0.18\n"
		"controller_zoom_speed=12.5\n"
		"invert_controller_pitch=true\n");
	fclose(file);

	assert(LNCT_LoadConfig(&config, path, sizeof(path)));
	assert(fabsf(config.controller_pitch_sensitivity - 0.18f) < 0.0001f);
	assert(fabsf(config.controller_zoom_speed - 12.5f) < 0.0001f);
	assert(fabsf(config.mouse_pitch_sensitivity - 1.50f) < 0.0001f);
	assert(config.invert_controller_pitch == 1);

	file = fopen(path, "r");
	assert(file);
	char content[1024] = {0};
	assert(fread(content, 1, sizeof(content) - 1, file) > 0);
	fclose(file);
	assert(strstr(content, "mouse_pitch_sensitivity=1.50"));

	file = fopen(path, "a");
	assert(file);
	fprintf(file, "mouse_pitch_sensitivity=1.25\n");
	fclose(file);

	assert(LNCT_LoadConfig(&config, path, sizeof(path)));
	assert(fabsf(config.mouse_pitch_sensitivity - 1.25f) < 0.0001f);
	assert(config.invert_controller_pitch == 1);
	return 0;
}

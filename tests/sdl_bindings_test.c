#include <assert.h>
#include <string.h>

#include "sdl_bindings.h"


int main(void)
{
	const char* json =
		"{"
		"\"CameraToggleMouseRotate\":[\"mouse:middle\",\"key:r\"],"
		"\"IgnoredControllerAction\":[\"c:leftstick\"]"
		"}";
	BindingSet bindings;
	assert(ParseBindingsJson(json, &bindings));

	const ActionBindings* rotate = FindAction(&bindings, "CameraToggleMouseRotate");
	assert(rotate);
	assert(rotate->binding_count == 2);
	assert(rotate->bindings[0].type == BINDING_MOUSE);
	assert(rotate->bindings[0].mouse_button == SDL_BUTTON_MIDDLE);
	assert(rotate->bindings[1].type == BINDING_KEY);
	assert(rotate->bindings[1].scancode == SDL_SCANCODE_R);

	Binding parsed;
	assert(!ParseBindingToken("mouse:not-a-button", &parsed));
	assert(!ParseBindingToken("key:not-a-key", &parsed));
	assert(!ParseBindingsJson("{\"CameraToggleMouseRotate\":[", &bindings));
	return 0;
}

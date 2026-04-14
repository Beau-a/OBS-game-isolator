#include <obs-module.h>
#include "pipewire-audio.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-pipewire-extended", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "PipeWire per-application audio stream capture";
}

bool obs_module_load(void)
{
	if (!pipewire_global_init()) {
		blog(LOG_ERROR,
		     "[obs-pipewire-extended] Failed to initialise PipeWire context");
		return false;
	}
	obs_register_source(&pipewire_audio_capture_info);
	blog(LOG_INFO, "[obs-pipewire-extended] Plugin loaded");
	return true;
}

void obs_module_unload(void)
{
	pipewire_global_destroy();
	blog(LOG_INFO, "[obs-pipewire-extended] Plugin unloaded");
}

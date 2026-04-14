#pragma once

#include <obs-module.h>
#include <stdbool.h>

/* Initialise the shared PipeWire context and node registry.
 * Called once from obs_module_load(). */
bool  pipewire_global_init(void);

/* Tear down the shared context. Called from obs_module_unload(). */
void  pipewire_global_destroy(void);

/* OBS source type descriptor — registered in plugin-main.c */
extern struct obs_source_info pipewire_audio_capture_info;

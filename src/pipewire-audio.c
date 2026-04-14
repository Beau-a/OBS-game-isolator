/*
 * obs-pipewire-extended  —  pipewire-audio.c
 *
 * Enumerates PipeWire audio output streams (one per running app) and exposes
 * each as an OBS audio source.  The user adds a "PipeWire Audio Capture"
 * source in OBS, picks the target application from a drop-down, and OBS
 * receives that app's audio on a dedicated track.
 *
 * Threading model
 * ---------------
 *   g_pw.loop  runs in its own thread (pw_thread_loop).
 *   All PipeWire API calls must be made with the loop lock held, EXCEPT
 *   inside PipeWire callbacks (which already run in the loop thread).
 *   obs_source_output_audio() is safe to call from any thread.
 */

#include "pipewire-audio.h"

#include <obs-module.h>
#include <util/platform.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Global node registry
 * ---------------------------------------------------------------------- */

#define MAX_NODES 128

struct audio_node {
	uint32_t id;
	char     display[256]; /* "AppName  (node.name)" shown in the UI   */
	char     key[256];     /* node.name — stable key stored in settings */
};

static struct {
	struct pw_thread_loop *loop;
	struct pw_context     *context;
	struct pw_core        *core;
	struct pw_registry    *registry;
	struct spa_hook        registry_hook;

	struct audio_node nodes[MAX_NODES];
	int               node_count;
	pthread_mutex_t   nodes_lock;

	bool initialized;
} g_pw;

/* -------------------------------------------------------------------------
 * Registry callbacks (run inside the PipeWire loop thread)
 * ---------------------------------------------------------------------- */

static void on_global(void *data, uint32_t id, uint32_t permissions,
		      const char *type, uint32_t version,
		      const struct spa_dict *props)
{
	(void)data;
	(void)permissions;
	(void)version;

	if (!props || strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
		return;

	const char *media_class = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
	if (!media_class ||
	    strcmp(media_class, "Stream/Output/Audio") != 0)
		return;

	const char *app_name  = spa_dict_lookup(props, PW_KEY_APP_NAME);
	const char *node_name = spa_dict_lookup(props, PW_KEY_NODE_NAME);

	if (!node_name)
		return; /* need a stable key */

	pthread_mutex_lock(&g_pw.nodes_lock);

	if (g_pw.node_count < MAX_NODES) {
		struct audio_node *n = &g_pw.nodes[g_pw.node_count++];
		n->id = id;

		snprintf(n->key, sizeof(n->key), "%s", node_name);

		if (app_name && strcmp(app_name, node_name) != 0)
			snprintf(n->display, sizeof(n->display),
				 "%s  (%s)", app_name, node_name);
		else
			snprintf(n->display, sizeof(n->display),
				 "%s", node_name);
	}

	pthread_mutex_unlock(&g_pw.nodes_lock);
}

static void on_global_remove(void *data, uint32_t id)
{
	(void)data;

	pthread_mutex_lock(&g_pw.nodes_lock);

	for (int i = 0; i < g_pw.node_count; i++) {
		if (g_pw.nodes[i].id == id) {
			memmove(&g_pw.nodes[i], &g_pw.nodes[i + 1],
				(g_pw.node_count - i - 1) *
					sizeof(struct audio_node));
			g_pw.node_count--;
			break;
		}
	}

	pthread_mutex_unlock(&g_pw.nodes_lock);
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global        = on_global,
	.global_remove = on_global_remove,
};

/* -------------------------------------------------------------------------
 * Global init / destroy
 * ---------------------------------------------------------------------- */

bool pipewire_global_init(void)
{
	pw_init(NULL, NULL);
	pthread_mutex_init(&g_pw.nodes_lock, NULL);

	g_pw.loop = pw_thread_loop_new("obs-pipewire-extended", NULL);
	if (!g_pw.loop)
		goto err_loop;

	g_pw.context = pw_context_new(
		pw_thread_loop_get_loop(g_pw.loop), NULL, 0);
	if (!g_pw.context)
		goto err_context;

	if (pw_thread_loop_start(g_pw.loop) < 0)
		goto err_start;

	pw_thread_loop_lock(g_pw.loop);

	g_pw.core = pw_context_connect(g_pw.context, NULL, 0);
	if (!g_pw.core) {
		pw_thread_loop_unlock(g_pw.loop);
		goto err_core;
	}

	g_pw.registry = pw_core_get_registry(
		g_pw.core, PW_VERSION_REGISTRY, 0);
	if (!g_pw.registry) {
		pw_thread_loop_unlock(g_pw.loop);
		goto err_registry;
	}

	pw_registry_add_listener(g_pw.registry, &g_pw.registry_hook,
				 &registry_events, NULL);

	pw_thread_loop_unlock(g_pw.loop);

	g_pw.initialized = true;
	return true;

err_registry:
	pw_core_disconnect(g_pw.core);
err_core:
	pw_thread_loop_stop(g_pw.loop);
err_start:
	pw_context_destroy(g_pw.context);
err_context:
	pw_thread_loop_destroy(g_pw.loop);
err_loop:
	pthread_mutex_destroy(&g_pw.nodes_lock);
	return false;
}

void pipewire_global_destroy(void)
{
	if (!g_pw.initialized)
		return;

	pw_thread_loop_lock(g_pw.loop);
	spa_hook_remove(&g_pw.registry_hook);
	pw_proxy_destroy((struct pw_proxy *)g_pw.registry);
	pw_core_disconnect(g_pw.core);
	pw_thread_loop_unlock(g_pw.loop);

	pw_thread_loop_stop(g_pw.loop);
	pw_context_destroy(g_pw.context);
	pw_thread_loop_destroy(g_pw.loop);
	pthread_mutex_destroy(&g_pw.nodes_lock);

	g_pw.initialized = false;
}

/* -------------------------------------------------------------------------
 * Per-source instance
 * ---------------------------------------------------------------------- */

struct pipewire_source {
	obs_source_t     *obs_source;

	struct pw_stream *stream;
	struct spa_hook   stream_hook;

	char     target_key[256]; /* node.name of the chosen stream     */
	uint32_t target_id;       /* PipeWire node ID, or PW_ID_ANY     */
	bool     stream_active;

	/* Negotiated format */
	uint32_t channels;
	uint32_t sample_rate;
};

/* -------------------------------------------------------------------------
 * PipeWire stream callbacks (run in the loop thread)
 * ---------------------------------------------------------------------- */

static void on_param_changed(void *userdata, uint32_t id,
			     const struct spa_pod *param)
{
	struct pipewire_source *ps = userdata;
	if (!param || id != SPA_PARAM_Format)
		return;

	struct spa_audio_info_raw info = {0};
	if (spa_format_audio_raw_parse(param, &info) < 0)
		return;

	ps->channels    = info.channels;
	ps->sample_rate = info.rate;

	blog(LOG_INFO,
	     "[obs-pipewire-extended] stream '%s' negotiated: %u ch @ %u Hz",
	     ps->target_key, ps->channels, ps->sample_rate);
}

static void on_process(void *userdata)
{
	struct pipewire_source *ps = userdata;

	struct pw_buffer *b = pw_stream_dequeue_buffer(ps->stream);
	if (!b)
		return;

	struct spa_buffer *buf = b->buffer;

	/* Interleaved F32 — single data plane */
	if (!buf->datas[0].data || buf->datas[0].chunk->size == 0)
		goto done;

	uint32_t ch       = ps->channels    ? ps->channels    : 2;
	uint32_t rate     = ps->sample_rate ? ps->sample_rate : 48000;
	uint32_t n_frames = buf->datas[0].chunk->size / (sizeof(float) * ch);

	struct obs_source_audio audio = {0};
	audio.data[0]        = (const uint8_t *)buf->datas[0].data;
	audio.frames         = n_frames;
	audio.format         = AUDIO_FORMAT_FLOAT;
	audio.samples_per_sec = rate;
	audio.speakers       = ch >= 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
	audio.timestamp      = os_gettime_ns();

	obs_source_output_audio(ps->obs_source, &audio);

done:
	pw_stream_queue_buffer(ps->stream, b);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.param_changed = on_param_changed,
	.process       = on_process,
};

/* -------------------------------------------------------------------------
 * Connect / disconnect helpers
 * ---------------------------------------------------------------------- */

static uint32_t find_node_id(const char *key)
{
	uint32_t id = PW_ID_ANY;
	pthread_mutex_lock(&g_pw.nodes_lock);
	for (int i = 0; i < g_pw.node_count; i++) {
		if (strcmp(g_pw.nodes[i].key, key) == 0) {
			id = g_pw.nodes[i].id;
			break;
		}
	}
	pthread_mutex_unlock(&g_pw.nodes_lock);
	return id;
}

static void source_connect(struct pipewire_source *ps)
{
	if (!g_pw.initialized || !ps->target_key[0])
		return;

	ps->target_id = find_node_id(ps->target_key);

	if (ps->target_id == PW_ID_ANY) {
		blog(LOG_WARNING,
		     "[obs-pipewire-extended] node '%s' not found in registry",
		     ps->target_key);
		/* Node may appear later; we'll reconnect on update */
		return;
	}

	pw_thread_loop_lock(g_pw.loop);

	char id_str[32];
	snprintf(id_str, sizeof(id_str), "%u", ps->target_id);

	struct pw_properties *props = pw_properties_new(
		PW_KEY_MEDIA_TYPE,     "Audio",
		PW_KEY_MEDIA_CATEGORY, "Capture",
		PW_KEY_MEDIA_ROLE,     "Music",
		PW_KEY_TARGET_OBJECT,  id_str,
		NULL);

	ps->stream = pw_stream_new(g_pw.core, "obs-audio-capture", props);
	if (!ps->stream) {
		pw_thread_loop_unlock(g_pw.loop);
		blog(LOG_ERROR,
		     "[obs-pipewire-extended] pw_stream_new() failed");
		return;
	}

	pw_stream_add_listener(ps->stream, &ps->stream_hook,
			       &stream_events, ps);

	/* Request interleaved F32, stereo, 48 kHz.
	 * PipeWire will negotiate the closest match available. */
	uint8_t                buffer[1024];
	struct spa_pod_builder b =
		SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];

	params[0] = spa_format_audio_raw_build(
		&b, SPA_PARAM_EnumFormat,
		&SPA_AUDIO_INFO_RAW_INIT(.format   = SPA_AUDIO_FORMAT_F32,
					  .channels = 2,
					  .rate     = 48000));

	pw_stream_connect(ps->stream,
			  PW_DIRECTION_INPUT,
			  PW_ID_ANY,
			  PW_STREAM_FLAG_AUTOCONNECT |
				  PW_STREAM_FLAG_MAP_BUFFERS,
			  params, 1);

	ps->stream_active = true;

	pw_thread_loop_unlock(g_pw.loop);

	blog(LOG_INFO,
	     "[obs-pipewire-extended] connected to node '%s' (id=%u)",
	     ps->target_key, ps->target_id);
}

static void source_disconnect(struct pipewire_source *ps)
{
	if (!ps->stream_active || !ps->stream)
		return;

	pw_thread_loop_lock(g_pw.loop);
	pw_stream_destroy(ps->stream);
	ps->stream = NULL;
	ps->stream_active = false;
	pw_thread_loop_unlock(g_pw.loop);

	spa_zero(ps->stream_hook);
}

/* -------------------------------------------------------------------------
 * OBS source callbacks
 * ---------------------------------------------------------------------- */

static const char *get_name(void *type_data)
{
	(void)type_data;
	return "PipeWire Extended";
}

static void *source_create(obs_data_t *settings, obs_source_t *obs_source)
{
	struct pipewire_source *ps = bzalloc(sizeof(*ps));
	ps->obs_source  = obs_source;
	ps->channels    = 2;
	ps->sample_rate = 48000;

	const char *key = obs_data_get_string(settings, "target_node");
	if (key && *key)
		snprintf(ps->target_key, sizeof(ps->target_key), "%s", key);

	source_connect(ps);
	return ps;
}

static void source_destroy(void *data)
{
	struct pipewire_source *ps = data;
	source_disconnect(ps);
	bfree(ps);
}

static void source_update(void *data, obs_data_t *settings)
{
	struct pipewire_source *ps = data;
	const char *key = obs_data_get_string(settings, "target_node");

	if (!key || strcmp(key, ps->target_key) == 0)
		return;

	source_disconnect(ps);
	snprintf(ps->target_key, sizeof(ps->target_key), "%s", key);
	source_connect(ps);
}

static obs_properties_t *get_properties(void *data)
{
	(void)data;

	obs_properties_t *props = obs_properties_create();
	obs_property_t *list = obs_properties_add_list(
		props, "target_node", "Audio Stream",
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);

	pthread_mutex_lock(&g_pw.nodes_lock);
	for (int i = 0; i < g_pw.node_count; i++) {
		obs_property_list_add_string(
			list,
			g_pw.nodes[i].display,
			g_pw.nodes[i].key);
	}
	pthread_mutex_unlock(&g_pw.nodes_lock);

	return props;
}

static void get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "target_node", "");
}

/* -------------------------------------------------------------------------
 * Source descriptor
 * ---------------------------------------------------------------------- */

struct obs_source_info pipewire_audio_capture_info = {
	.id             = "pipewire_extended",
	.type           = OBS_SOURCE_TYPE_INPUT,
	.output_flags   = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name       = get_name,
	.create         = source_create,
	.destroy        = source_destroy,
	.update         = source_update,
	.get_properties = get_properties,
	.get_defaults   = get_defaults,
};

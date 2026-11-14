/*
 * obs-multistream -- module entry point.
 *
 * The plugin only reacts to the frontend's streaming lifecycle: when OBS
 * starts streaming it boots one extra RTMP output per configured target, and
 * when OBS stops it shuts them all down again.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>

#include "multistream.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-multistream", "en-US")

#define MS_LOG(level, ...) blog(level, "[obs-multistream] " __VA_ARGS__)

#ifndef PLUGIN_VERSION
#define PLUGIN_VERSION "0.0.0"
#endif

static bool g_dock_added;
static bool g_shut_down;

static void frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		/* Fallback: usually the dock is in place already, but if the
		 * module loaded before the main window was ready this is the
		 * point where it can finally be attached. */
		if (!g_dock_added)
			g_dock_added = ms_dock_register();
		break;

	case OBS_FRONTEND_EVENT_STREAMING_STARTING:
		ms_on_streaming_starting();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STARTED:
		ms_on_streaming_started();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPING:
		ms_on_streaming_stopping();
		break;
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		ms_on_streaming_stopped();
		break;

	case OBS_FRONTEND_EVENT_EXIT:
		/* Stop early: OBS is still fully alive here, which is the last
		 * moment it is safe to talk to outputs and services. */
		if (!g_shut_down) {
			g_shut_down = true;
			ms_core_shutdown();
		}
		break;

	default:
		break;
	}
}

bool obs_module_load(void)
{
	ms_core_init();

	obs_frontend_add_event_callback(frontend_event, NULL);
	g_dock_added = ms_dock_register();

	MS_LOG(LOG_INFO, "loaded version %s (%zu target(s) configured)", PLUGIN_VERSION,
	       ms_target_count());
	return true;
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(frontend_event, NULL);

	/* The dock is owned by OBS' main window and is destroyed with it, so
	 * there is deliberately nothing to unregister here. */
	ms_dock_unregister();

	if (!g_shut_down) {
		g_shut_down = true;
		ms_core_shutdown();
	}

	MS_LOG(LOG_INFO, "unloaded");
}

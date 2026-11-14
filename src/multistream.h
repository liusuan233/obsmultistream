/*
 * obs-multistream -- push one OBS stream to several RTMP/RTMPS endpoints.
 *
 * This header is deliberately OBS-free so the Qt dock can include it too.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MS_MAX_TARGETS 8
#define MS_MAX_TRACKS 6
#define MS_STR_LEN 512
#define MS_STATUS_LEN 160

/* How the extra output gets its encoders. */
enum ms_encoder_mode {
	/* Reuse the encoders of the main streaming output: no extra
	 * encoding cost at all. This is what OBS itself does when you
	 * "use stream encoder" for recording. */
	MS_ENC_SHARED = 0,

	/* Create private encoder instances that clone the main output's
	 * encoder id + settings. Costs a second encode pass. Useful when a
	 * platform rejects the main encoder's parameters (resolution or
	 * bitrate limits), because it can be configured independently. */
	MS_ENC_INDEPENDENT = 1,
};

/* ---------------------------------------------------------------- core --
 * Called by plugin-main.c only. */

void ms_core_init(void);
void ms_core_shutdown(void);

/* Persist the target list to the module config directory. */
void ms_core_save(void);

/* ------------------------------------------------- frontend event hooks -- */

void ms_on_streaming_starting(void);
void ms_on_streaming_started(void);
void ms_on_streaming_stopping(void);
void ms_on_streaming_stopped(void);

/* ------------------------------------------------------- target list --
 * All of these are safe to call from the GUI thread. `id` values are
 * stable for the lifetime of a target and are never reused. */

size_t ms_target_count(void);
uint64_t ms_target_id_at(size_t index);

/* Incremented whenever a target is added or removed, so the dock knows
 * when to rebuild its rows instead of just refreshing status text. */
uint64_t ms_generation(void);

/* Returns the new target id, or 0 when the list is full. */
uint64_t ms_target_add(const char *name, const char *url, const char *key);
void ms_target_remove(uint64_t id);

void ms_target_set_name(uint64_t id, const char *value);
void ms_target_set_url(uint64_t id, const char *value);
void ms_target_set_key(uint64_t id, const char *value);
void ms_target_set_enabled(uint64_t id, bool value);
void ms_target_set_mode(uint64_t id, int mode);
void ms_target_set_reconnect(uint64_t id, int seconds);

bool ms_target_get_name(uint64_t id, char *buf, size_t len);
bool ms_target_get_url(uint64_t id, char *buf, size_t len);
bool ms_target_get_key(uint64_t id, char *buf, size_t len);
bool ms_target_get_enabled(uint64_t id);
int ms_target_get_mode(uint64_t id);
int ms_target_get_reconnect(uint64_t id);

bool ms_target_is_running(uint64_t id);
bool ms_target_get_status(uint64_t id, char *buf, size_t len);

/* Start/stop a single target right now, without waiting for the next
 * streaming session. Both are no-ops when OBS is not streaming. */
void ms_target_start_now(uint64_t id);
void ms_target_stop_now(uint64_t id);

/* ------------------------------------------------------------- dock UI -- */

/* Returns false when the frontend is not ready to host a dock yet. */
bool ms_dock_register(void);
void ms_dock_unregister(void);

#ifdef __cplusplus
}
#endif

/*
 * obs-multistream -- core: target model, config, output lifecycle.
 *
 * Design notes
 * ------------
 * Each target owns one `rtmp_output` plus one `rtmp_custom` service. The
 * outputs are created lazily and then *reused* across streaming sessions,
 * because an OBS output cannot be safely released while its own thread may
 * still be finishing a stop. Anything that is retired goes to a small
 * graveyard and is only released once `obs_output_active()` says it is done.
 *
 * Locking: `g_lock` guards the target array. No OBS call that can emit a
 * signal or block is ever made while holding it; start/stop work happens on
 * snapshot values outside the lock. Signal callbacks only touch status text.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <util/threading.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "multistream.h"

#define MS_LOG(level, ...) blog(level, "[obs-multistream] " __VA_ARGS__)

struct ms_target {
	uint64_t id;
	char *name;
	char *url;
	char *key;
	bool enabled;
	int mode;
	int reconnect_sec;

	/* runtime, owned by the target until it is retired */
	obs_output_t *output;
	obs_service_t *service;

	char status[MS_STATUS_LEN];
	bool running;
};

struct ms_retired {
	obs_output_t *output;
	obs_service_t *service;
	uint64_t id;
};

static struct ms_target g_targets[MS_MAX_TARGETS];
static size_t g_count;
static uint64_t g_next_id = 1;
static uint64_t g_generation = 1;
static pthread_mutex_t g_lock;

static struct ms_retired g_retired[MS_MAX_TARGETS * 2];
static size_t g_retired_count;

/* ------------------------------------------------------------- helpers -- */

static void str_assign(char **dst, const char *src)
{
	char *copy = bstrdup(src ? src : "");
	bfree(*dst);
	*dst = copy;
}

static struct ms_target *find_locked(uint64_t id)
{
	for (size_t i = 0; i < g_count; i++)
		if (g_targets[i].id == id)
			return &g_targets[i];
	return NULL;
}

static void set_status_locked(struct ms_target *t, const char *text)
{
	snprintf(t->status, sizeof(t->status), "%s", text ? text : "");
}

/* Locale strings use OBS' %1 placeholder, which is *not* a printf format,
 * so substituting it by hand keeps snprintf out of untrusted format strings. */
static void status_with_arg(char *buf, size_t len, const char *key, const char *arg)
{
	const char *tmpl = obs_module_text(key);
	const char *pos = strstr(tmpl ? tmpl : "", "%1");

	if (!pos) {
		snprintf(buf, len, "%s", tmpl ? tmpl : "");
		return;
	}

	size_t head = (size_t)(pos - tmpl);
	if (head >= len)
		head = len ? len - 1 : 0;

	if (head)
		memcpy(buf, tmpl, head);
	snprintf(buf + head, len - head, "%s%s", arg ? arg : "", pos + 2);
}

static void update_locked(uint64_t id, bool running, const char *text)
{
	struct ms_target *t = find_locked(id);
	if (!t)
		return;
	t->running = running;
	if (text)
		set_status_locked(t, text);
}

static void update_status(uint64_t id, bool running, const char *text)
{
	pthread_mutex_lock(&g_lock);
	update_locked(id, running, text);
	pthread_mutex_unlock(&g_lock);
}

/* ------------------------------------------------ output signal hooks --
 * The target id is passed as the callback data. This relies on uintptr_t
 * being at least 64 bits, which holds on every platform OBS still ships. */

static uint64_t id_of(void *data)
{
	return (uint64_t)(uintptr_t)data;
}

static void on_output_started(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	update_status(id_of(data), true, obs_module_text("Status.Live"));
}

static void on_output_deactivate(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
}

static void on_output_reconnecting(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	update_status(id_of(data), true, obs_module_text("Status.Reconnecting"));
}

static void on_output_reconnected(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	update_status(id_of(data), true, obs_module_text("Status.Live"));
}

static void on_output_stopped(void *data, calldata_t *cd)
{
	int code = 0;
	char text[MS_STATUS_LEN];

	if (cd)
		code = (int)calldata_int(cd, "code");

	if (code != 0) {
		const char *err = cd ? calldata_string(cd, "last_error") : NULL;
		status_with_arg(text, sizeof(text), "Status.StoppedWithError",
				err && *err ? err : "unknown");
	} else {
		snprintf(text, sizeof(text), "%s", obs_module_text("Status.Stopped"));
	}

	update_status(id_of(data), false, text);
}

static void connect_output_signals(obs_output_t *output, uint64_t id)
{
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	void *data = (void *)(uintptr_t)id;

	if (!sh || id == 0)
		return;

	signal_handler_connect(sh, "started", on_output_started, data);
	signal_handler_connect(sh, "stopped", on_output_stopped, data);
	signal_handler_connect(sh, "activate", on_output_deactivate, data);
	signal_handler_connect(sh, "deactivate", on_output_deactivate, data);
	signal_handler_connect(sh, "reconnect", on_output_reconnecting, data);
	signal_handler_connect(sh, "reconnect_success", on_output_reconnected, data);
}

static void disconnect_output_signals(obs_output_t *output, uint64_t id)
{
	signal_handler_t *sh = obs_output_get_signal_handler(output);
	void *data = (void *)(uintptr_t)id;

	if (!sh || id == 0)
		return;

	signal_handler_disconnect(sh, "started", on_output_started, data);
	signal_handler_disconnect(sh, "stopped", on_output_stopped, data);
	signal_handler_disconnect(sh, "activate", on_output_deactivate, data);
	signal_handler_disconnect(sh, "deactivate", on_output_deactivate, data);
	signal_handler_disconnect(sh, "reconnect", on_output_reconnecting, data);
	signal_handler_disconnect(sh, "reconnect_success", on_output_reconnected, data);
}

/* ---------------------------------------------------------- retirement -- */

static void retire_locked(struct ms_target *t)
{
	if (!t->output && !t->service)
		return;

	if (g_retired_count < sizeof(g_retired) / sizeof(g_retired[0])) {
		struct ms_retired *slot = &g_retired[g_retired_count++];
		slot->output = t->output;
		slot->service = t->service;
		slot->id = t->id;
	} else {
		/* Graceyard full: leaving the objects alive beats risking a
		 * use-after-free from an output thread. */
		MS_LOG(LOG_WARNING, "retirement queue full, leaking output %llu",
		       (unsigned long long)t->id);
	}

	t->output = NULL;
	t->service = NULL;
}

static void drain_retired(void)
{
	struct ms_retired ready[sizeof(g_retired) / sizeof(g_retired[0])];
	size_t ready_count = 0;

	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < g_retired_count;) {
		if (!g_retired[i].output || !obs_output_active(g_retired[i].output)) {
			ready[ready_count++] = g_retired[i];
			for (size_t j = i; j + 1 < g_retired_count; j++)
				g_retired[j] = g_retired[j + 1];
			g_retired_count--;
		} else {
			i++;
		}
	}
	pthread_mutex_unlock(&g_lock);

	for (size_t i = 0; i < ready_count; i++) {
		if (ready[i].output) {
			disconnect_output_signals(ready[i].output, ready[i].id);
			obs_output_release(ready[i].output);
			MS_LOG(LOG_DEBUG, "released output %llu",
			       (unsigned long long)ready[i].id);
		}
		if (ready[i].service)
			obs_service_release(ready[i].service);
	}
}

/* ----------------------------------------------------------- encoders -- */

static bool wire_shared(obs_output_t *output, obs_output_t *main_output)
{
	obs_encoder_t *venc = obs_output_get_video_encoder(main_output);

	if (!venc) {
		MS_LOG(LOG_ERROR, "main streaming output has no video encoder");
		return false;
	}

	/* obs_output_set_video_encoder() takes its own reference (libobs does
	 * obs_encoder_get_ref() + obs_encoder_add_output()), and the getters
	 * above hand out *borrowed* pointers -- so nothing is released here. */
	obs_output_set_video_encoder(output, venc);

	for (size_t i = 0; i < MS_MAX_TRACKS; i++) {
		obs_encoder_t *aenc = obs_output_get_audio_encoder(main_output, i);
		if (!aenc)
			continue;
		obs_output_set_audio_encoder(output, aenc, i);
	}

	return true;
}

static bool wire_independent(obs_output_t *output, obs_output_t *main_output,
			     uint64_t id)
{
	obs_encoder_t *main_venc = obs_output_get_video_encoder(main_output);
	if (!main_venc)
		return false;

	/* All getters below return borrowed pointers; only the settings come
	 * back with their own reference, so only obs_data_release() is due. */
	const char *vid = obs_encoder_get_id(main_venc);
	obs_data_t *vsettings = obs_encoder_get_settings(main_venc);
	uint32_t width = obs_encoder_get_width(main_venc);
	uint32_t height = obs_encoder_get_height(main_venc);
	char vname[64];
	snprintf(vname, sizeof(vname), "multistream_venc_%llu",
		 (unsigned long long)id);

	obs_encoder_t *venc = obs_video_encoder_create(vid, vname, vsettings, NULL);
	obs_data_release(vsettings);

	if (!venc) {
		MS_LOG(LOG_ERROR, "could not clone video encoder '%s'", vid);
		return false;
	}

	/* The scaled size lives on the encoder object, not in its settings:
	 * copy it so the clone matches what the main stream actually sends. */
	if (width && height)
		obs_encoder_set_scaled_size(venc, width, height);

	obs_encoder_set_video(venc, obs_get_video());
	obs_output_set_video_encoder(output, venc);
	obs_encoder_release(venc);

	for (size_t i = 0; i < MS_MAX_TRACKS; i++) {
		obs_encoder_t *main_aenc = obs_output_get_audio_encoder(main_output, i);
		if (!main_aenc)
			continue;

		const char *aid = obs_encoder_get_id(main_aenc);
		obs_data_t *asettings = obs_encoder_get_settings(main_aenc);
		char aname[64];
		snprintf(aname, sizeof(aname), "multistream_aenc_%llu_%zu",
			 (unsigned long long)id, i);

		obs_encoder_t *aenc =
			obs_audio_encoder_create(aid, aname, asettings, i, NULL);
		obs_data_release(asettings);

		if (!aenc)
			continue;

		obs_encoder_set_audio(aenc, obs_get_audio());
		obs_output_set_audio_encoder(output, aenc, i);
		obs_encoder_release(aenc);
	}

	return true;
}

/* ------------------------------------------------------------ lifetime -- */

static bool ensure_runtime_locked(struct ms_target *t)
{
	if (!t->service) {
		obs_data_t *settings = obs_data_create();
		char name[64];
		snprintf(name, sizeof(name), "multistream_svc_%llu",
			 (unsigned long long)t->id);
		t->service = obs_service_create("rtmp_custom", name, settings, NULL);
		obs_data_release(settings);

		if (!t->service) {
			MS_LOG(LOG_ERROR, "obs_service_create(rtmp_custom) failed");
			return false;
		}
	}

	if (!t->output) {
		obs_data_t *settings = obs_data_create();
		char name[64];
		snprintf(name, sizeof(name), "multistream_out_%llu",
			 (unsigned long long)t->id);
		t->output = obs_output_create("rtmp_output", name, settings, NULL);
		obs_data_release(settings);

		if (!t->output) {
			MS_LOG(LOG_ERROR, "obs_output_create(rtmp_output) failed");
			return false;
		}

		connect_output_signals(t->output, t->id);
	}

	obs_output_set_service(t->output, t->service);
	return true;
}

static void start_one(uint64_t id, obs_output_t *main_output)
{
	char url[MS_STR_LEN] = "";
	char key[MS_STR_LEN] = "";
	int mode = MS_ENC_SHARED;
	int reconnect = 2;
	obs_output_t *output = NULL;
	obs_service_t *service = NULL;
	bool abort = false;

	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (!t || !t->enabled) {
		pthread_mutex_unlock(&g_lock);
		return;
	}
	if (t->running || (t->output && obs_output_active(t->output))) {
		pthread_mutex_unlock(&g_lock);
		return;
	}

	snprintf(url, sizeof(url), "%s", t->url ? t->url : "");
	snprintf(key, sizeof(key), "%s", t->key ? t->key : "");
	mode = t->mode;
	reconnect = t->reconnect_sec;

	if (!url[0] || !key[0]) {
		set_status_locked(t, obs_module_text("Status.NotConfigured"));
		abort = true;
	} else if (!ensure_runtime_locked(t)) {
		set_status_locked(t, obs_module_text("Status.Error"));
		abort = true;
	} else {
		output = obs_output_get_ref(t->output);
		service = obs_service_get_ref(t->service);
		set_status_locked(t, obs_module_text("Status.Connecting"));
	}
	pthread_mutex_unlock(&g_lock);

	if (abort)
		return;

	obs_data_t *ssettings = obs_data_create();
	obs_data_set_string(ssettings, "server", url);
	obs_data_set_string(ssettings, "key", key);
	obs_service_update(service, ssettings);
	obs_data_release(ssettings);

	bool wired = (mode == MS_ENC_INDEPENDENT)
			     ? wire_independent(output, main_output, id)
			     : wire_shared(output, main_output);

	if (!wired) {
		/* Distinguish "the main stream has nothing to copy" from "the
		 * encoder for this target could not be created": the two need
		 * very different fixes from the user. */
		const char *why = obs_output_get_video_encoder(main_output)
					  ? obs_module_text("Error.EncoderCreate")
					  : obs_module_text("Error.NoVideoEncoder");
		update_status(id, false, why);
		goto done;
	}

	obs_output_set_reconnect_settings(output, 20, reconnect > 0 ? reconnect : 2);

	if (obs_output_start(output)) {
		/* Status text is driven by the output's own signals from here on:
		 * "started" once the handshake is done, "reconnect" while retrying. */
		update_status(id, true, NULL);
		MS_LOG(LOG_INFO, "target %llu: started (%s encoders)",
		       (unsigned long long)id,
		       mode == MS_ENC_INDEPENDENT ? "independent" : "shared");
	} else {
		const char *err = obs_output_get_last_error(output);
		char text[MS_STATUS_LEN];
		status_with_arg(text, sizeof(text), "Status.StartFailed",
				err && *err ? err : "unknown");
		update_status(id, false, text);
		MS_LOG(LOG_ERROR, "target %llu failed to start: %s",
		       (unsigned long long)id, err && *err ? err : "unknown");
	}

done:
	obs_output_release(output);
	obs_service_release(service);
}

static void stop_one(uint64_t id)
{
	obs_output_t *output = NULL;

	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t && t->output)
		output = obs_output_get_ref(t->output);
	if (t) {
		t->running = false;
		set_status_locked(t, obs_module_text("Status.Stopping"));
	}
	pthread_mutex_unlock(&g_lock);

	if (!output)
		return;

	if (obs_output_active(output)) {
		obs_output_stop(output);
		MS_LOG(LOG_INFO, "target %llu: stop requested", (unsigned long long)id);
	} else {
		update_status(id, false, obs_module_text("Status.Stopped"));
	}
	obs_output_release(output);
}

static void snapshot_ids(uint64_t *ids, size_t *count)
{
	pthread_mutex_lock(&g_lock);
	*count = g_count;
	for (size_t i = 0; i < g_count; i++)
		ids[i] = g_targets[i].id;
	pthread_mutex_unlock(&g_lock);
}

static void stop_all(void)
{
	uint64_t ids[MS_MAX_TARGETS];
	size_t count = 0;

	snapshot_ids(ids, &count);
	for (size_t i = 0; i < count; i++)
		stop_one(ids[i]);
}

/* ------------------------------------------------------- frontend hooks -- */

void ms_on_streaming_starting(void)
{
	drain_retired();
}

void ms_on_streaming_started(void)
{
	/* obs_frontend_get_streaming_output() has been documented differently
	 * across OBS releases (borrowed in some, owning in others), so we do
	 * not adopt or drop the pointer it hands back: we pin the object with
	 * a reference of our own and release exactly that one at the end. That
	 * is balanced whether the frontend returned borrowed or owning. */
	obs_output_t *main_output =
		obs_output_get_ref(obs_frontend_get_streaming_output());
	uint64_t ids[MS_MAX_TARGETS];
	size_t count = 0;

	if (!main_output) {
		MS_LOG(LOG_WARNING, "no main streaming output, skipping multi-stream");
		return;
	}

	snapshot_ids(ids, &count);
	for (size_t i = 0; i < count; i++)
		start_one(ids[i], main_output);

	obs_output_release(main_output);
	drain_retired();
}

void ms_on_streaming_stopping(void)
{
	stop_all();
}

void ms_on_streaming_stopped(void)
{
	stop_all();
	drain_retired();
}

/* Used by the dock when a target is added or enabled mid-stream, so the user
 * does not have to restart the main stream to bring an extra platform up. */
void ms_target_start_now(uint64_t id)
{
	obs_output_t *main_output;

	if (!obs_frontend_streaming_active())
		return;

	main_output = obs_output_get_ref(obs_frontend_get_streaming_output());
	if (!main_output)
		return;

	start_one(id, main_output);
	obs_output_release(main_output);
}

void ms_target_stop_now(uint64_t id)
{
	stop_one(id);
	drain_retired();
}

/* ----------------------------------------------------------- target CRUD -- */

static uint64_t add_locked(const char *name, const char *url, const char *key)
{
	if (g_count >= MS_MAX_TARGETS)
		return 0;

	struct ms_target *t = &g_targets[g_count];
	memset(t, 0, sizeof(*t));
	t->id = g_next_id++;
	str_assign(&t->name, name);
	str_assign(&t->url, url);
	str_assign(&t->key, key);
	t->enabled = true;
	t->mode = MS_ENC_SHARED;
	t->reconnect_sec = 2;
	set_status_locked(t, obs_module_text("Status.Idle"));

	g_count++;
	g_generation++;
	return t->id;
}

size_t ms_target_count(void)
{
	size_t count;
	pthread_mutex_lock(&g_lock);
	count = g_count;
	pthread_mutex_unlock(&g_lock);
	return count;
}

uint64_t ms_target_id_at(size_t index)
{
	uint64_t id = 0;
	pthread_mutex_lock(&g_lock);
	if (index < g_count)
		id = g_targets[index].id;
	pthread_mutex_unlock(&g_lock);
	return id;
}

uint64_t ms_generation(void)
{
	uint64_t generation;
	pthread_mutex_lock(&g_lock);
	generation = g_generation;
	pthread_mutex_unlock(&g_lock);
	return generation;
}

uint64_t ms_target_add(const char *name, const char *url, const char *key)
{
	uint64_t id;

	pthread_mutex_lock(&g_lock);
	id = add_locked(name, url, key);
	pthread_mutex_unlock(&g_lock);

	if (id)
		ms_core_save();
	return id;
}

void ms_target_remove(uint64_t id)
{
	obs_output_t *output = NULL;
	bool found = false;

	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t) {
		found = true;
		size_t index = (size_t)(t - g_targets);
		if (t->output)
			output = obs_output_get_ref(t->output);
		retire_locked(t);
		bfree(t->name);
		bfree(t->url);
		bfree(t->key);
		t->name = t->url = t->key = NULL;

		for (size_t i = index; i + 1 < g_count; i++)
			g_targets[i] = g_targets[i + 1];
		g_count--;
		memset(&g_targets[g_count], 0, sizeof(g_targets[0]));
		g_generation++;
	}
	pthread_mutex_unlock(&g_lock);

	if (!found)
		return;

	if (output) {
		if (obs_output_active(output))
			obs_output_stop(output);
		obs_output_release(output);
	}

	drain_retired();
	ms_core_save();
}

/* ------------------------------------------------------- field accessors -- */

#define MS_SIMPLE_SETTER(fn, field)                                      \
	void fn(uint64_t id, const char *value)                          \
	{                                                                \
		pthread_mutex_lock(&g_lock);                              \
		struct ms_target *t = find_locked(id);                    \
		if (t)                                                    \
			str_assign(&t->field, value);                     \
		pthread_mutex_unlock(&g_lock);                            \
		ms_core_save();                                           \
	}

MS_SIMPLE_SETTER(ms_target_set_name, name)
MS_SIMPLE_SETTER(ms_target_set_url, url)
MS_SIMPLE_SETTER(ms_target_set_key, key)

void ms_target_set_enabled(uint64_t id, bool value)
{
	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t)
		t->enabled = value;
	pthread_mutex_unlock(&g_lock);
	ms_core_save();
}

void ms_target_set_mode(uint64_t id, int mode)
{
	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t)
		t->mode = (mode == MS_ENC_INDEPENDENT) ? MS_ENC_INDEPENDENT
						      : MS_ENC_SHARED;
	pthread_mutex_unlock(&g_lock);
	ms_core_save();
}

void ms_target_set_reconnect(uint64_t id, int seconds)
{
	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t)
		t->reconnect_sec = seconds < 1 ? 1 : (seconds > 30 ? 30 : seconds);
	pthread_mutex_unlock(&g_lock);
	ms_core_save();
}

#define MS_SIMPLE_GETTER(fn, field)                                      \
	bool fn(uint64_t id, char *buf, size_t len)                      \
	{                                                                \
		bool ok = false;                                          \
		if (!buf || !len)                                         \
			return false;                                     \
		pthread_mutex_lock(&g_lock);                              \
		struct ms_target *t = find_locked(id);                    \
		if (t) {                                                  \
			snprintf(buf, len, "%s", t->field ? t->field : ""); \
			ok = true;                                        \
		}                                                         \
		pthread_mutex_unlock(&g_lock);                             \
		return ok;                                                \
	}

MS_SIMPLE_GETTER(ms_target_get_name, name)
MS_SIMPLE_GETTER(ms_target_get_url, url)
MS_SIMPLE_GETTER(ms_target_get_key, key)

bool ms_target_get_enabled(uint64_t id)
{
	bool value = false;
	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t)
		value = t->enabled;
	pthread_mutex_unlock(&g_lock);
	return value;
}

int ms_target_get_mode(uint64_t id)
{
	int value = MS_ENC_SHARED;
	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t)
		value = t->mode;
	pthread_mutex_unlock(&g_lock);
	return value;
}

int ms_target_get_reconnect(uint64_t id)
{
	int value = 2;
	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t)
		value = t->reconnect_sec;
	pthread_mutex_unlock(&g_lock);
	return value;
}

bool ms_target_is_running(uint64_t id)
{
	bool value = false;
	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t)
		value = t->running;
	pthread_mutex_unlock(&g_lock);
	return value;
}

bool ms_target_get_status(uint64_t id, char *buf, size_t len)
{
	bool ok = false;
	if (!buf || !len)
		return false;
	pthread_mutex_lock(&g_lock);
	struct ms_target *t = find_locked(id);
	if (t) {
		snprintf(buf, len, "%s", t->status);
		ok = true;
	}
	pthread_mutex_unlock(&g_lock);
	return ok;
}

/* ---------------------------------------------------------------- config -- */

static void ensure_config_dir(void)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
}

void ms_core_save(void)
{
	obs_data_t *root = obs_data_create();
	obs_data_array_t *array = obs_data_array_create();

	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < g_count; i++) {
		struct ms_target *t = &g_targets[i];
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", t->name ? t->name : "");
		obs_data_set_string(item, "url", t->url ? t->url : "");
		obs_data_set_string(item, "key", t->key ? t->key : "");
		obs_data_set_bool(item, "enabled", t->enabled);
		obs_data_set_int(item, "mode", t->mode);
		obs_data_set_int(item, "reconnect", t->reconnect_sec);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&g_lock);

	obs_data_set_array(root, "targets", array);
	obs_data_array_release(array);

	ensure_config_dir();
	char *path = obs_module_config_path("targets.json");
	if (path) {
		obs_data_save_json_safe(root, path, "tmp", "bak");
		bfree(path);
	}
	obs_data_release(root);
}

static void core_load(void)
{
	char *path = obs_module_config_path("targets.json");
	obs_data_t *root = NULL;

	if (!path)
		return;

	root = obs_data_create_from_json_file_safe(path, "bak");
	bfree(path);

	if (root) {
		obs_data_array_t *array = obs_data_get_array(root, "targets");
		if (array) {
			size_t count = obs_data_array_count(array);
			pthread_mutex_lock(&g_lock);
			for (size_t i = 0; i < count && g_count < MS_MAX_TARGETS; i++) {
				obs_data_t *item = obs_data_array_item(array, i);
				char *name = bstrdup(obs_data_get_string(item, "name"));
				char *url = bstrdup(obs_data_get_string(item, "url"));
				char *key = bstrdup(obs_data_get_string(item, "key"));
				uint64_t id = add_locked(name, url, key);
				if (id) {
					struct ms_target *t = find_locked(id);
					t->enabled = obs_data_get_bool(item, "enabled");
					t->mode = (int)obs_data_get_int(item, "mode");
					t->reconnect_sec =
						(int)obs_data_get_int(item, "reconnect");
				}
				bfree(name);
				bfree(url);
				bfree(key);
				obs_data_release(item);
			}
			pthread_mutex_unlock(&g_lock);
			obs_data_array_release(array);
		}
		obs_data_release(root);
	}

	if (ms_target_count() == 0) {
		pthread_mutex_lock(&g_lock);
		add_locked(obs_module_text("Default.Name"), "", "");
		pthread_mutex_unlock(&g_lock);
	}
}

void ms_core_init(void)
{
	pthread_mutex_init(&g_lock, NULL);
	core_load();
}

void ms_core_shutdown(void)
{
	stop_all();
	drain_retired();
	ms_core_save();

	pthread_mutex_lock(&g_lock);
	for (size_t i = 0; i < g_count; i++) {
		struct ms_target *t = &g_targets[i];
		if (t->output) {
			disconnect_output_signals(t->output, t->id);
			obs_output_release(t->output);
			t->output = NULL;
		}
		if (t->service) {
			obs_service_release(t->service);
			t->service = NULL;
		}
		bfree(t->name);
		bfree(t->url);
		bfree(t->key);
		t->name = t->url = t->key = NULL;
	}
	g_count = 0;
	pthread_mutex_unlock(&g_lock);

	for (size_t i = 0; i < g_retired_count; i++) {
		if (g_retired[i].output)
			obs_output_release(g_retired[i].output);
		if (g_retired[i].service)
			obs_service_release(g_retired[i].service);
	}
	g_retired_count = 0;

	pthread_mutex_destroy(&g_lock);
}

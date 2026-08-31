/*
 * obs-streamtranslate — streams an OBS audio source to StreamTranslate for
 * real-time translated captions, from inside any OBS instance (including
 * cloud-hosted ones like IRLToolkit where no local browser exists).
 *
 * Design: an audio FILTER you attach to the audio source you stream with.
 * It passes audio through untouched, and in parallel converts it to mono
 * s16le at the OBS pipeline sample rate and ships it over a TLS websocket
 * to wss://<server>/audio?pluginKey=...&rate=<hz>. The plugin key is the
 * only credential — generate it from your StreamTranslate account.
 *
 * Linux-compilable (IRLToolkit third-party plugin requirement).
 * Deps: libobs, libwebsockets.
 */

#include <obs-module.h>
#include <media-io/audio-resampler.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <util/threading.h>
#include <libwebsockets.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#endif

OBS_DECLARE_MODULE()

#define ST_QUEUE_MAX 256 /* ~30s of 120ms chunks — drop oldest beyond this */

struct st_chunk {
	struct st_chunk *next;
	size_t len;
	unsigned char *buf; /* includes LWS_PRE headroom; payload at buf+LWS_PRE */
};

struct st_filter {
	obs_source_t *context;

	/* config */
	char server[256];
	char plugin_key[128];

	/* live status shown in the filter UI */
	char status[320];
	bool muted_now;
	int  audio_chunks_captured;
	int  audio_chunks_sent;

	/* audio conversion */
	audio_resampler_t *resampler;
	uint32_t sample_rate;

	/* websocket service thread */
	pthread_t thread;
	volatile bool stop;
	volatile bool connected;
	struct lws_context *lws_ctx;
	struct lws *wsi;

	/* outbound queue (audio thread -> ws thread) */
	pthread_mutex_t qlock;
	struct st_chunk *qhead, *qtail;
	int qcount;
};

static void st_set_status(struct st_filter *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(f->status, sizeof(f->status), fmt, ap);
	va_end(ap);
	blog(LOG_INFO, "[streamtranslate] %s", f->status);
	/* OBS builds the properties panel once; without this the Status line
	 * shows whatever it said when the dialog opened. */
	if (f->context) {
		obs_data_t *sd = obs_source_get_settings(f->context);
		if (sd) {
			obs_data_set_string(sd, "status_text", f->status);
			obs_data_release(sd);
		}
		obs_source_update_properties(f->context);
	}
}

/* ---------------- queue ---------------- */

static void st_queue_clear(struct st_filter *f)
{
	pthread_mutex_lock(&f->qlock);
	struct st_chunk *c = f->qhead;
	while (c) {
		struct st_chunk *n = c->next;
		free(c->buf);
		free(c);
		c = n;
	}
	f->qhead = f->qtail = NULL;
	f->qcount = 0;
	pthread_mutex_unlock(&f->qlock);
}

static void st_queue_push(struct st_filter *f, const unsigned char *data, size_t len)
{
	struct st_chunk *c = malloc(sizeof(*c));
	if (!c)
		return;
	c->buf = malloc(LWS_PRE + len);
	if (!c->buf) {
		free(c);
		return;
	}
	memcpy(c->buf + LWS_PRE, data, len);
	c->len = len;
	c->next = NULL;

	pthread_mutex_lock(&f->qlock);
	if (f->qcount >= ST_QUEUE_MAX && f->qhead) {
		struct st_chunk *old = f->qhead;
		f->qhead = old->next;
		if (!f->qhead)
			f->qtail = NULL;
		f->qcount--;
		free(old->buf);
		free(old);
	}
	if (f->qtail)
		f->qtail->next = c;
	else
		f->qhead = c;
	f->qtail = c;
	f->qcount++;
	pthread_mutex_unlock(&f->qlock);
}

static struct st_chunk *st_queue_pop(struct st_filter *f)
{
	pthread_mutex_lock(&f->qlock);
	struct st_chunk *c = f->qhead;
	if (c) {
		f->qhead = c->next;
		if (!f->qhead)
			f->qtail = NULL;
		f->qcount--;
	}
	pthread_mutex_unlock(&f->qlock);
	return c;
}

/* ---------------- websocket ---------------- */

static int st_ws_callback(struct lws *wsi, enum lws_callback_reasons reason,
			  void *user, void *in, size_t len)
{
	struct st_filter *f = lws_context_user(lws_get_context(wsi));
	(void)user;
	(void)in;
	(void)len;
	if (!f)
		return 0;

	switch (reason) {
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		st_set_status(f, "Connected to %s - streaming audio", f->server);
		f->connected = true;
		lws_callback_on_writable(wsi);
		break;

	case LWS_CALLBACK_CLIENT_WRITEABLE: {
		struct st_chunk *c = st_queue_pop(f);
		if (c) {
			int wrote = lws_write(wsi, c->buf + LWS_PRE, c->len, LWS_WRITE_BINARY);
			if (wrote > 0)
				f->audio_chunks_sent++;
			free(c->buf);
			free(c);
			lws_callback_on_writable(wsi);
		}
		break;
	}

	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		if (f->wsi && f->connected)
			lws_callback_on_writable(f->wsi);
		break;

	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		st_set_status(f, "Connection FAILED: %s (server: %s) - retrying",
			      in ? (const char *)in : "unknown error", f->server);
		f->connected = false;
		f->wsi = NULL;
		break;

	case LWS_CALLBACK_CLIENT_CLOSED:
		st_set_status(f, "Disconnected from %s - reconnecting", f->server);
		f->connected = false;
		f->wsi = NULL;
		break;

	default:
		break;
	}
	return 0;
}

static const struct lws_protocols st_protocols[] = {
	{"st-audio", st_ws_callback, 0, 4096, 0, NULL, 0},
	LWS_PROTOCOL_LIST_TERM,
};

static void st_connect(struct st_filter *f)
{
	if (!f->lws_ctx || f->wsi)
		return;
	if (!f->plugin_key[0]) {
		st_set_status(f, "Not configured - paste your Plugin Key above");
		return;
	}

	char path[512];
	snprintf(path, sizeof(path), "/audio?pluginKey=%s&rate=%u",
		 f->plugin_key, f->sample_rate);

	struct lws_client_connect_info ci;
	memset(&ci, 0, sizeof(ci));
	ci.context = f->lws_ctx;
	ci.address = f->server;
	ci.port = 443;
	ci.path = path;
	ci.host = f->server;
	ci.origin = f->server;
	ci.ssl_connection = LCCSCF_USE_SSL;
	ci.protocol = NULL; /* server doesn't negotiate a subprotocol */
	ci.local_protocol_name = "st-audio";

	st_set_status(f, "Connecting to %s ...", f->server);
	f->wsi = lws_client_connect_via_info(&ci);
	if (!f->wsi)
		st_set_status(f, "Could not start connection to %s - check the Server field", f->server);
}

/* TLS trust: the bundled OpenSSL has no OS certificate store, so server
 * verification fails silently without a CA file. We ship Mozilla's CA bundle
 * (cacert.pem) inside the plugin and point lws at it. Resolved relative to
 * this module's own binary location. */
static const char *st_ca_bundle_path(void)
{
	static char path[1024];
	static int resolved = 0;
	if (resolved)
		return path[0] ? path : NULL;
	resolved = 1;
	path[0] = 0;
#ifdef _WIN32
	HMODULE hm = NULL;
	if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			       (LPCSTR)&st_ca_bundle_path, &hm)) {
		char mod[1024];
		if (GetModuleFileNameA(hm, mod, sizeof(mod))) {
			char *slash = strrchr(mod, '\\');
			if (slash) {
				*slash = 0;
				snprintf(path, sizeof(path), "%s\\cacert.pem", mod);
			}
		}
	}
#else
	Dl_info dli;
	if (dladdr((void *)&st_ca_bundle_path, &dli) && dli.dli_fname) {
		char mod[1024];
		snprintf(mod, sizeof(mod), "%s", dli.dli_fname);
		char *slash = strrchr(mod, '/');
		if (slash) {
			*slash = 0;
			/* macOS bundle: .../Contents/MacOS -> .../Contents/Resources/cacert.pem */
			char *macos = strstr(mod, "/Contents/MacOS");
			if (macos) {
				*macos = 0;
				snprintf(path, sizeof(path), "%s/Contents/Resources/cacert.pem", mod);
			} else {
				snprintf(path, sizeof(path), "%s/cacert.pem", mod);
			}
		}
	}
	if (path[0] && access(path, R_OK) != 0) {
		/* fall back to common system stores (Linux/cloud OBS) */
		if (access("/etc/ssl/certs/ca-certificates.crt", R_OK) == 0)
			snprintf(path, sizeof(path), "/etc/ssl/certs/ca-certificates.crt");
		else
			path[0] = 0;
	}
#endif
	if (path[0])
		blog(LOG_INFO, "[streamtranslate] CA bundle: %s", path);
	else
		blog(LOG_WARNING, "[streamtranslate] no CA bundle found — TLS verification may fail");
	return path[0] ? path : NULL;
}

static void *st_ws_thread(void *arg)
{
	struct st_filter *f = arg;
	uint64_t next_retry = 0;

	struct lws_context_creation_info info;
	memset(&info, 0, sizeof(info));
	info.port = CONTEXT_PORT_NO_LISTEN;
	info.protocols = st_protocols;
	info.options = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	info.user = f;
	info.client_ssl_ca_filepath = st_ca_bundle_path();

	f->lws_ctx = lws_create_context(&info);
	if (!f->lws_ctx) {
		blog(LOG_ERROR, "[streamtranslate] lws context creation failed");
		return NULL;
	}

	uint64_t last_status = 0;
	while (!f->stop) {
		if (!f->wsi) {
			uint64_t now = os_gettime_ns();
			if (now >= next_retry) {
				st_connect(f);
				next_retry = now + 3000000000ULL; /* 3s backoff */
			}
		}
		/* Ask for a writeable slot from THIS thread whenever audio is waiting.
		 * (Relying on lws_cancel_service from the audio thread proved unreliable:
		 * the socket connected but never sent a byte, so the server's watchdog
		 * closed the silent connection after ~30s.) */
		if (f->wsi && f->connected) {
			pthread_mutex_lock(&f->qlock);
			int pending = f->qcount;
			pthread_mutex_unlock(&f->qlock);
			if (pending > 0)
				lws_callback_on_writable(f->wsi);
		}
		lws_service(f->lws_ctx, 20);

		/* refresh the status line with live counters once a second */
		uint64_t now2 = os_gettime_ns();
		if (f->connected && !f->muted_now && now2 - last_status > 1000000000ULL) {
			last_status = now2;
			st_set_status(f, "Connected to %s - captured %d, sent %d audio chunks",
				      f->server, f->audio_chunks_captured, f->audio_chunks_sent);
		}
	}

	if (f->wsi) {
		lws_set_timeout(f->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
		lws_service(f->lws_ctx, 50);
	}
	lws_context_destroy(f->lws_ctx);
	f->lws_ctx = NULL;
	f->wsi = NULL;
	f->connected = false;
	return NULL;
}

/* ---------------- OBS filter ---------------- */

static const char *st_get_name(void *unused)
{
	(void)unused;
	return "StreamTranslate (live translated captions)";
}

static void st_build_resampler(struct st_filter *f)
{
	if (f->resampler) {
		audio_resampler_destroy(f->resampler);
		f->resampler = NULL;
	}
	const struct audio_output_info *aoi = audio_output_get_info(obs_get_audio());
	if (!aoi)
		return;
	f->sample_rate = aoi->samples_per_sec;

	struct resample_info src = {
		.samples_per_sec = aoi->samples_per_sec,
		.format = AUDIO_FORMAT_FLOAT_PLANAR,
		.speakers = aoi->speakers,
	};
	struct resample_info dst = {
		.samples_per_sec = aoi->samples_per_sec,
		.format = AUDIO_FORMAT_16BIT,
		.speakers = SPEAKERS_MONO,
	};
	f->resampler = audio_resampler_create(&dst, &src);
	if (!f->resampler)
		blog(LOG_ERROR, "[streamtranslate] resampler creation failed");
}

static void st_update(void *data, obs_data_t *settings)
{
	struct st_filter *f = data;
	const char *server = obs_data_get_string(settings, "server");
	const char *key = obs_data_get_string(settings, "plugin_key");

	bool changed = strcmp(f->server, server ? server : "") != 0 ||
		       strcmp(f->plugin_key, key ? key : "") != 0;

	snprintf(f->server, sizeof(f->server), "%s", server ? server : "");
	snprintf(f->plugin_key, sizeof(f->plugin_key), "%s", key ? key : "");

	if (changed && f->wsi) {
		/* reconnect with new credentials: close current, thread will redial */
		lws_set_timeout(f->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
		lws_cancel_service(f->lws_ctx);
	}
}

static void *st_create(obs_data_t *settings, obs_source_t *context)
{
	struct st_filter *f = bzalloc(sizeof(struct st_filter));
	f->context = context;
	pthread_mutex_init(&f->qlock, NULL);
	st_build_resampler(f);
	st_update(f, settings);
	f->stop = false;
	pthread_create(&f->thread, NULL, st_ws_thread, f);
	return f;
}

static void st_destroy(void *data)
{
	struct st_filter *f = data;
	f->stop = true;
	if (f->lws_ctx)
		lws_cancel_service(f->lws_ctx);
	pthread_join(f->thread, NULL);
	st_queue_clear(f);
	pthread_mutex_destroy(&f->qlock);
	if (f->resampler)
		audio_resampler_destroy(f->resampler);
	bfree(f);
}

static struct obs_audio_data *st_filter_audio(void *data, struct obs_audio_data *audio)
{
	struct st_filter *f = data;
	if (!audio || !audio->frames)
		return audio;

	/* OBS applies a source's mute AFTER its filter chain, so a muted mic still
	 * reaches us. Without this check a streamer who mutes for a private moment
	 * would still be transcribed and captioned on stream. Also honour the filter's
	 * own enabled toggle (the eye icon) as a pause control. */
	obs_source_t *parent = obs_filter_get_parent(f->context);
	bool blocked = (parent && obs_source_muted(parent)) || !obs_source_enabled(f->context);
	if (blocked) {
		if (!f->muted_now) {
			f->muted_now = true;
			st_set_status(f, "Muted in OBS - not sending audio (captured %d, sent %d)",
				      f->audio_chunks_captured, f->audio_chunks_sent);
		}
		return audio;
	}
	if (f->muted_now)
		f->muted_now = false;
	if (!f->resampler) {
		st_build_resampler(f); /* audio subsystem may not have been ready at create time */
		if (!f->resampler)
			return audio;
	}
	if (!f->connected)
		return audio; /* always pass audio through untouched */

	uint8_t *out[MAX_AV_PLANES] = {0};
	uint32_t out_frames = 0;
	uint64_t ts_offset = 0;

	if (audio_resampler_resample(f->resampler, out, &out_frames, &ts_offset,
				     (const uint8_t *const *)audio->data,
				     audio->frames) &&
	    out_frames > 0 && out[0]) {
		st_queue_push(f, out[0], (size_t)out_frames * 2 /* s16 mono */);
		f->audio_chunks_captured++;
		if (f->lws_ctx)
			lws_cancel_service(f->lws_ctx); /* wake ws thread to flush */
	}
	return audio;
}

static bool st_reconnect_clicked(obs_properties_t *props, obs_property_t *prop, void *data)
{
	struct st_filter *f = data;
	(void)props;
	(void)prop;
	if (!f)
		return false;
	st_set_status(f, "Reconnecting ...");
	if (f->wsi) {
		lws_set_timeout(f->wsi, PENDING_TIMEOUT_CLOSE_SEND, LWS_TO_KILL_ASYNC);
	}
	if (f->lws_ctx)
		lws_cancel_service(f->lws_ctx);
	return true;
}

static obs_properties_t *st_get_properties(void *data)
{
	struct st_filter *f = data;
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_text(props, "plugin_key",
				"Plugin Key (from your StreamTranslate account)",
				OBS_TEXT_PASSWORD);
	obs_properties_add_text(props, "server", "Server", OBS_TEXT_DEFAULT);

	/* live status — updated by the connection thread */
	obs_property_t *st = obs_properties_add_text(props, "status_text", "Status", OBS_TEXT_INFO);
	if (f) {
		char line[420];
		snprintf(line, sizeof(line), "%s%s", f->status[0] ? f->status : "Starting up ...",
			 f->connected ? "" : "");
		obs_property_set_long_description(st, line);
		obs_data_t *s = obs_source_get_settings(f->context);
		if (s) {
			obs_data_set_string(s, "status_text", line);
			obs_data_release(s);
		}
	}

	obs_properties_add_button2(props, "reconnect", "Reconnect / Test connection",
				   st_reconnect_clicked, f);
	return props;
}

static void st_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "server", "streamtranslate.live");
	obs_data_set_default_string(settings, "plugin_key", "");
}

static struct obs_source_info st_filter_info = {
	.id = "streamtranslate_audio_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_AUDIO,
	.get_name = st_get_name,
	.create = st_create,
	.destroy = st_destroy,
	.update = st_update,
	.filter_audio = st_filter_audio,
	.get_properties = st_get_properties,
	.get_defaults = st_get_defaults,
};

bool obs_module_load(void)
{
	obs_register_source(&st_filter_info);
	blog(LOG_INFO, "[streamtranslate] plugin loaded");
	return true;
}

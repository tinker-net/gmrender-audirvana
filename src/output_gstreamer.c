/* output_gstreamer.c - Output module for GStreamer
 *
 * Copyright (C) 2005-2007   Ivo Clarysse
 *
 * Adapted to gstreamer-0.10 2006 David Siorpaes
 * Adapted to output to snapcast 2017 Daniel Jäcksch
 *
 * This file is part of GMediaRender.
 *
 * GMediaRender is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * GMediaRender is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GMediaRender; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <gst/gst.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <inttypes.h>

#include "logging.h"
#include "upnp_connmgr.h"
#include "output_module.h"
#include "output_gstreamer.h"
#include "dsd_output.h"
#include "alsa_volume.h"

static gchar *dsd_mode = NULL;
static gboolean fixed_volume = FALSE;
static gchar *alsa_mixer_name = NULL;
static gint http_retries = 3;
static gint http_timeout = 15;

static double buffer_duration = 0.0; /* Buffer disbled by default, see #182 */

static void scan_mime_list(void)
{
	GstRegistry* registry = NULL;

#if (GST_VERSION_MAJOR < 1)
	registry = gst_registry_get_default();
#else
	registry = gst_registry_get();
#endif

	// Fetch a list of all element factories
	GList* features =
		gst_registry_get_feature_list(registry, GST_TYPE_ELEMENT_FACTORY);

	// Save a copy of the list root so we can properly free later
	GList* root = features;

	while (features != NULL) {
		GstPluginFeature* feature = GST_PLUGIN_FEATURE(features->data);

		// Advance list
		features = g_list_next(features);

		// Better be an element factory
		assert(GST_IS_ELEMENT_FACTORY(feature));

		GstElementFactory* factory = GST_ELEMENT_FACTORY(feature);

		// Ignore elements without pads
		if (gst_element_factory_get_num_pad_templates(factory) == 0) continue;

		// Fetch a list of all pads
		const GList* pads = gst_element_factory_get_static_pad_templates(factory);

		while (pads) {
			GstStaticPadTemplate* padTemplate = (GstStaticPadTemplate*)pads->data;

			// Advance list
			pads = g_list_next(pads);

			// Skip pads that aren't sinks
			if (padTemplate->direction != GST_PAD_SINK) continue;

			// This is literally all known pad presences so it should be OK!
			assert(padTemplate->presence == GST_PAD_ALWAYS ||
				   padTemplate->presence == GST_PAD_SOMETIMES ||
				   padTemplate->presence == GST_PAD_REQUEST);

			GstCaps* capabilities = gst_static_caps_get(&padTemplate->static_caps);

			// Skip capabilities that they tell us nothing
			if (capabilities == NULL || gst_caps_is_any(capabilities) ||
				gst_caps_is_empty(capabilities))
			{
				gst_caps_unref(capabilities);
				continue;
			}

			for (guint i = 0; i < gst_caps_get_size(capabilities); i++) {
				GstStructure* structure = gst_caps_get_structure(capabilities, i);

				const char *mime = gst_structure_get_name(structure);
				if (strstr(mime, "dsd") || strstr(mime, "dff") || strstr(mime, "dsf")) continue;
				register_mime_type(mime);
			}

			gst_caps_unref(capabilities);
		}
	}

	// Free any allocated memory
	gst_plugin_feature_list_free(root);

	// There seem to be all kinds of mime types out there that start with
	// "audio/" but are not explicitly supported by gstreamer. Let's just
	// tell the controller that we can handle everything "audio/*" and hope
	// for the best.
	/* WAV aliases explicitly required by some UPnP clients. */
	GstElementFactory *wav = gst_element_factory_find("wavparse");
	if (wav) {
		register_mime_type("audio/wav");
		register_mime_type("audio/x-wav");
		gst_object_unref(wav);
	}
}

static GstElement *player_ = NULL;
static char *gsuri_ = NULL;         // locally strdup()ed
static char *gs_next_uri_ = NULL;   // locally strdup()ed
static char *gs_next_meta_ = NULL;
static char *gs_pending_uri_ = NULL;
static char *gs_pending_meta_ = NULL;
static GMutex uri_lock_;
static gboolean stream_seen_ = FALSE;
static gboolean pending_started_ = FALSE;
static guint current_group_id_ = 0;
static struct SongMetaData song_meta_;

static output_transition_cb_t play_trans_callback_ = NULL;
static output_update_meta_cb_t meta_update_callback_ = NULL;

struct track_time_info {
	gint64 duration;
	gint64 position;
};
static struct track_time_info last_known_time_ = {0, 0};

/* Separate user intent from temporary GStreamer buffering states. */
enum PlaybackIntent { INTENT_STOPPED, INTENT_PLAYING, INTENT_PAUSED };
static gint playback_intent_ = INTENT_STOPPED;
static gint uri_changed_ = TRUE;

static GstState get_current_player_state() {
	GstState state = GST_STATE_PLAYING;
	GstState pending = GST_STATE_NULL;
	gst_element_get_state(player_, &state, &pending, 0);
	return state;
}

static void clear_pending_locked(void) {
	g_clear_pointer(&gs_pending_uri_, g_free);
	g_clear_pointer(&gs_pending_meta_, g_free);
	pending_started_ = FALSE;
}

static int output_gstreamer_play(output_transition_cb_t callback) {
	g_mutex_lock(&uri_lock_);
	char *uri = g_strdup(gsuri_);
	play_trans_callback_ = callback;
	g_mutex_unlock(&uri_lock_);
	if (!uri) return -1;
	if (g_atomic_int_get(&uri_changed_) || get_current_player_state() != GST_STATE_PAUSED) {
		if (gst_element_set_state(player_, GST_STATE_READY) ==
		    GST_STATE_CHANGE_FAILURE) {
			Log_error("gstreamer", "setting play state failed (1)");
			g_atomic_int_set(&playback_intent_, INTENT_STOPPED);
			g_free(uri);
			return -1;
		}
		g_mutex_lock(&uri_lock_);
		clear_pending_locked();
		stream_seen_ = FALSE;
		current_group_id_ = 0;
		g_mutex_unlock(&uri_lock_);
		g_object_set(G_OBJECT(player_), "uri", uri, NULL);
		g_atomic_int_set(&uri_changed_, FALSE);
	}
	g_free(uri);
	g_atomic_int_set(&playback_intent_, INTENT_PLAYING);
	if (gst_element_set_state(player_, GST_STATE_PLAYING) ==
	    GST_STATE_CHANGE_FAILURE) {
		Log_error("gstreamer", "setting play state failed (2)");
		g_atomic_int_set(&playback_intent_, INTENT_STOPPED);
		g_atomic_int_set(&uri_changed_, TRUE);
		return -1;
	}
	return 0;
}

static int output_gstreamer_stop(void) {
	g_atomic_int_set(&playback_intent_, INTENT_STOPPED);
	g_atomic_int_set(&uri_changed_, TRUE);
	g_mutex_lock(&uri_lock_);
	clear_pending_locked();
	stream_seen_ = FALSE;
	g_mutex_unlock(&uri_lock_);
	if (gst_element_set_state(player_, GST_STATE_READY) ==
	    GST_STATE_CHANGE_FAILURE) {
		return -1;
	} else {
		return 0;
	}
}

static int output_gstreamer_pause(void) {
	g_atomic_int_set(&playback_intent_, INTENT_PAUSED);
	if (gst_element_set_state(player_, GST_STATE_PAUSED) ==
	    GST_STATE_CHANGE_FAILURE) {
		g_atomic_int_set(&playback_intent_, INTENT_PLAYING);
		return -1;
	} else {
		return 0;
	}
}

static int output_gstreamer_seek(gint64 position_nanos) {
	if (gst_element_seek_simple(player_, GST_FORMAT_TIME, GST_SEEK_FLAG_FLUSH,
								position_nanos)) {
		return 0;
	} else {
		return -1;
	}
}

static void output_gstreamer_set_next_uri(const char *uri, const char *meta) {
	Log_info("gstreamer", "Set next uri to '%s'", uri);
	g_mutex_lock(&uri_lock_);
	g_free(gs_next_uri_); g_free(gs_next_meta_);
	gs_next_uri_ = (uri && *uri) ? g_strdup(uri) : NULL;
	gs_next_meta_ = gs_next_uri_ ? g_strdup(meta ? meta : "") : NULL;
	g_mutex_unlock(&uri_lock_);
}

static void output_gstreamer_set_uri(const char *uri,
				     output_update_meta_cb_t meta_cb) {
	Log_info("gstreamer", "Set uri to '%s'", uri);
	g_mutex_lock(&uri_lock_);
	g_free(gsuri_);
	gsuri_ = (uri && *uri) ? g_strdup(uri) : NULL;
	clear_pending_locked();
	g_mutex_unlock(&uri_lock_);
	last_known_time_.duration = 0;
	last_known_time_.position = 0;
	g_atomic_int_set(&uri_changed_, TRUE);
	meta_update_callback_ = meta_cb;
	SongMetaData_clear(&song_meta_);

	/* A cleared URI must never restart playbin with a NULL resource. */
	if (!uri || !*uri) return;
	// If already playing, update the playbin's URI
	if (g_atomic_int_get(&playback_intent_) == INTENT_PLAYING)
		output_gstreamer_play(play_trans_callback_);
}

#if 0
static const char *gststate_get_name(GstState state)
{
	switch(state) {
	case GST_STATE_VOID_PENDING:
		return "VOID_PENDING";
	case GST_STATE_NULL:
		return "NULL";
	case GST_STATE_READY:
		return "READY";
	case GST_STATE_PAUSED:
		return "PAUSED";
	case GST_STATE_PLAYING:
		return "PLAYING";
	default:
		return "Unknown";
	}
}
#endif

// This is crazy. I want C++ :)
struct MetaModify {
	struct SongMetaData *meta;
	int any_change;
};

static const char kStreamUrlTag[] = "stream-url";

static int is_image_uri(const char *uri)
{
	const char *end = strpbrk(uri, "?#");
	const char *path_end = end ? end : uri + strlen(uri);
	const char *extension = NULL;
	const char *cursor = path_end;
	while (cursor > uri && cursor[-1] != '/') {
		if (cursor[-1] == '.') {
			extension = cursor - 1;
			break;
		}
		cursor--;
	}
	if (extension == NULL) {
		return 0;
	}
	return (path_end - extension == 4 &&
		strncasecmp(extension, ".jpg", 4) == 0) ||
	       (path_end - extension == 5 &&
		strncasecmp(extension, ".jpeg", 5) == 0) ||
	       (path_end - extension == 4 &&
		strncasecmp(extension, ".png", 4) == 0) ||
	       (path_end - extension == 4 &&
		strncasecmp(extension, ".gif", 4) == 0) ||
	       (path_end - extension == 5 &&
		strncasecmp(extension, ".webp", 5) == 0);
}

static void MetaModify_add_tag(const GstTagList *list, const gchar *tag,
			       gpointer user_data) {
	struct MetaModify *data = (struct MetaModify*) user_data;
	const char **destination = NULL;
	if (strcmp(tag, GST_TAG_TITLE) == 0) {
		destination = &data->meta->title;
	} else if (strcmp(tag, GST_TAG_ARTIST) == 0) {
		destination = &data->meta->artist;
	} else if (strcmp(tag, GST_TAG_ALBUM) == 0) {
		destination = &data->meta->album;
	} else if (strcmp(tag, GST_TAG_GENRE) == 0) {
		destination = &data->meta->genre;
	} else if (strcmp(tag, GST_TAG_COMPOSER) == 0) {
		destination = &data->meta->composer;
	} else if (strcmp(tag, kStreamUrlTag) == 0 ||
		   strcmp(tag, GST_TAG_HOMEPAGE) == 0) {
		destination = &data->meta->album_art_uri;
	}
	if (destination != NULL) {
		char *replace = NULL;
		gst_tag_list_get_string(list, tag, &replace);
		if (destination == &data->meta->album_art_uri &&
		    (replace == NULL || !is_image_uri(replace))) {
			free(replace);
			return;
		}
		if (replace != NULL &&
		    (*destination == NULL
		     || strcmp(replace, *destination) != 0)) {
			free((char*)*destination);
			*destination = replace;
			data->any_change++;
		} else {
			free(replace);
		}
	}
}

static gboolean my_bus_callback(GstBus * bus, GstMessage * msg,
				gpointer data)
{
	(void)bus;
	(void)data;

	GstMessageType msgType;
	const GstObject *msgSrc;
	const gchar *msgSrcName;

	msgType = GST_MESSAGE_TYPE(msg);
	msgSrc = GST_MESSAGE_SRC(msg);
	msgSrcName = GST_OBJECT_NAME(msgSrc);

	switch (msgType) {
	case GST_MESSAGE_STREAM_START: {
		/* about-to-finish is a prefetch signal, not a track boundary.
		 * Commit the track only when GStreamer reports its new stream. */
		guint group = 0;
		gst_message_parse_group_id(msg, &group);
		char *uri = NULL, *meta = NULL;
		output_transition_cb_t callback = NULL;
		g_mutex_lock(&uri_lock_);
		if (g_atomic_int_get(&playback_intent_) != INTENT_STOPPED &&
		    (!stream_seen_ || !group || group != current_group_id_)) {
			if (gs_pending_uri_ && (stream_seen_ || pending_started_)) {
				g_free(gsuri_);
				gsuri_ = gs_pending_uri_;
				gs_pending_uri_ = NULL;
				uri = g_strdup(gsuri_);
				meta = gs_pending_meta_;
				gs_pending_meta_ = NULL;
				pending_started_ = FALSE;
				callback = play_trans_callback_;
			}
			stream_seen_ = TRUE;
			current_group_id_ = group;
		}
		g_mutex_unlock(&uri_lock_);
		if (uri && callback) callback(PLAY_STARTED_NEXT_STREAM, uri, meta);
		g_free(uri); g_free(meta);
		break;
	}
	case GST_MESSAGE_EOS: {
		Log_info("gstreamer", "%s: End-of-stream", msgSrcName);
		if (g_atomic_int_get(&playback_intent_) == INTENT_STOPPED) break;
		/* Late queue entries still play, but require a new preroll. */
		g_mutex_lock(&uri_lock_);
		if (!gs_pending_uri_ && gs_next_uri_) {
			gs_pending_uri_ = gs_next_uri_; gs_next_uri_ = NULL;
			gs_pending_meta_ = gs_next_meta_; gs_next_meta_ = NULL;
		}
		char *next = g_strdup(gs_pending_uri_);
		pending_started_ = next != NULL;
		stream_seen_ = FALSE;
		current_group_id_ = 0;
		output_transition_cb_t callback = play_trans_callback_;
		g_mutex_unlock(&uri_lock_);
		if (next) {
			gst_element_set_state(player_, GST_STATE_READY);
			g_object_set(player_, "uri", next, NULL);
			if (gst_element_set_state(player_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
				g_atomic_int_set(&playback_intent_, INTENT_STOPPED);
				if (callback) callback(PLAY_FAILED, NULL, NULL);
			}
		} else {
			g_atomic_int_set(&playback_intent_, INTENT_STOPPED);
			g_atomic_int_set(&uri_changed_, TRUE);
			if (callback) callback(PLAY_STOPPED, NULL, NULL);
		}
		g_free(next);
		break;
	}

	case GST_MESSAGE_ERROR: {
		gchar *debug;
		GError *err;

		gst_message_parse_error(msg, &err, &debug);

		Log_error("gstreamer", "%s: Error: %s (Debug: %s)",
			  msgSrcName, err->message, debug);
		g_error_free(err);
		g_free(debug);

		/* Errors are not EOS. Preserve the transport URI for a controller
		 * retry, and ignore subsequent EOS from this failed stream. */
		if (g_atomic_int_compare_and_exchange(&playback_intent_, INTENT_PLAYING, INTENT_STOPPED) ||
		    g_atomic_int_compare_and_exchange(&playback_intent_, INTENT_PAUSED, INTENT_STOPPED)) {
			g_atomic_int_set(&uri_changed_, TRUE);
			gst_element_set_state(player_, GST_STATE_READY);
			g_mutex_lock(&uri_lock_);
			clear_pending_locked();
			stream_seen_ = FALSE;
			output_transition_cb_t callback = play_trans_callback_;
			g_mutex_unlock(&uri_lock_);
			if (callback) callback(PLAY_FAILED, NULL, NULL);
		}
		break;
	}
	case GST_MESSAGE_STATE_CHANGED: {
		GstState oldstate, newstate, pending;
		gst_message_parse_state_changed(msg, &oldstate, &newstate,
						&pending);
		/*
		g_print("GStreamer: %s: State change: '%s' -> '%s', "
			"PENDING: '%s'\n", msgSrcName,
			gststate_get_name(oldstate),
			gststate_get_name(newstate),
			gststate_get_name(pending));
		*/
		break;
	}

	case GST_MESSAGE_TAG: {
		GstTagList *tags = NULL;

		if (meta_update_callback_ != NULL) {
			gst_message_parse_tag(msg, &tags);
			/*g_print("GStreamer: Got tags from element %s\n",
				GST_OBJECT_NAME (msg->src));
			*/
			struct MetaModify modify;
			modify.meta = &song_meta_;
			modify.any_change = 0;
			gst_tag_list_foreach(tags, &MetaModify_add_tag, &modify);
			gst_tag_list_free(tags);
			if (modify.any_change) {
				meta_update_callback_(&song_meta_);
			}
		}
		break;
	}

	case GST_MESSAGE_BUFFERING:
        {
                if (buffer_duration <= 0.0) break;  /* nothing to buffer */
                if (g_atomic_int_get(&playback_intent_) != INTENT_PLAYING) break;

                gint percent = 0;
                gst_message_parse_buffering (msg, &percent);


                /* Pause playback until buffering is complete. */
                if (percent < 100)
                        gst_element_set_state(player_, GST_STATE_PAUSED);
                else
                        gst_element_set_state(player_, GST_STATE_PLAYING);
		break;
        }
	default:
		/*
		g_print("GStreamer: %s: unhandled message type %d (%s)\n",
		        msgSrcName, msgType, gst_message_type_get_name(msgType));
		*/
		break;
	}

	return TRUE;
}

static gchar *audio_sink = NULL;
static gchar *audio_device = NULL;
static gchar *audio_pipe = NULL;
static gchar *video_sink = NULL;
static gchar *video_pipe = NULL;
static double initial_db = 0.0;

/* Options specific to output_gstreamer */
static GOptionEntry option_entries[] = {
        { "gstout-audiosink", 0, 0, G_OPTION_ARG_STRING, &audio_sink,
          "GStreamer audio sink to use "
	  "(autoaudiosink, alsasink, osssink, esdsink, ...)",
	  NULL },
        { "gstout-audiodevice", 0, 0, G_OPTION_ARG_STRING, &audio_device,
          "GStreamer device for the given audiosink. ",
	  NULL },
        { "gstout-audiopipe", 0, 0, G_OPTION_ARG_STRING, &audio_pipe,
          "GStreamer audio sink to pipeline"
          "(gst-launch format) useful for further output format conversion.",
	  NULL },
        { "gstout-videosink", 0, 0, G_OPTION_ARG_STRING, &video_sink,
          "GStreamer video sink to use "
	  "(autovideosink, xvimagesink, ximagesink, ...)",
	  NULL },
        { "gstout-videopipe", 0, 0, G_OPTION_ARG_STRING, &video_pipe,
          "GStreamer video sink to pipeline"
          "(gst-launch format) useful for further output format conversion.",
	  NULL },
        { "gstout-buffer-duration", 0, 0, G_OPTION_ARG_DOUBLE, &buffer_duration,
          "The size of the buffer in seconds. Set to zero to disable buffering.",
          NULL },
        { "gstout-dsd-mode", 0, 0, G_OPTION_ARG_STRING, &dsd_mode,
          "DSD output: off, native or dop (requires DFF parser; direct ALSA)", NULL },
        { "gstout-fixed-volume", 0, 0, G_OPTION_ARG_NONE, &fixed_volume,
          "Disable digital attenuation and resampling; use external volume control", NULL },
        { "gstout-alsa-mixer-control", 0, 0, G_OPTION_ARG_STRING, &alsa_mixer_name,
          "ALSA mixer name to control volume while keeping GStreamer at unity", NULL },
        { "gstout-http-retries", 0, 0, G_OPTION_ARG_INT, &http_retries,
          "Bounded souphttpsrc retries (0 disables)", NULL },
        { "gstout-http-timeout", 0, 0, G_OPTION_ARG_INT, &http_timeout,
          "HTTP I/O timeout in seconds", NULL },
        { "gstout-initial-volume-db", 0, 0, G_OPTION_ARG_DOUBLE, &initial_db,
          "GStreamer initial volume in decibel (e.g. 0.0 = max; -6 = 1/2 max) ",
	  NULL },
        { NULL }
};


static int output_gstreamer_add_options(GOptionContext *ctx)
{
	GOptionGroup *option_group;
	option_group = g_option_group_new("gstout", "GStreamer Output Options",
	                                  "Show GStreamer Output Options",
	                                  NULL, NULL);
	g_option_group_add_entries(option_group, option_entries);

	g_option_context_add_group (ctx, option_group);

	g_option_context_add_group (ctx, gst_init_get_option_group ());
	return 0;
}

static int output_gstreamer_get_position(gint64 *track_duration,
					 gint64 *track_pos) {
	*track_duration = last_known_time_.duration;
	*track_pos = last_known_time_.position;

	int rc = 0;
	if (get_current_player_state() != GST_STATE_PLAYING) {
		return rc;  // playbin2 only returns valid values then.
	}
#if (GST_VERSION_MAJOR < 1)
	GstFormat fmt = GST_FORMAT_TIME;
	GstFormat* query_type = &fmt;
#else
	GstFormat query_type = GST_FORMAT_TIME;
#endif
	if (!gst_element_query_duration(player_, query_type, track_duration)) {
		Log_error("gstreamer", "Failed to get track duration.");
		rc = -1;
	}
	if (!gst_element_query_position(player_, query_type, track_pos)) {
		Log_error("gstreamer", "Failed to get track pos");
		rc = -1;
	}
	// playbin2 does not allow to query while paused. Remember in case
	// we're asked then (it actually returns something, but it is bogus).
	last_known_time_.duration = *track_duration;
	last_known_time_.position = *track_pos;
	return rc;
}

static int output_gstreamer_get_volume(float *v) {
	if (gm_alsa_volume_enabled()) {
		double fraction;
		if (gm_alsa_volume_get(&fraction)) return -1;
		*v = (float)fraction;
		return 0;
	}
	if (fixed_volume) { *v = 1.0f; return 0; }
	double volume;
	g_object_get(player_, "volume", &volume, NULL);
	Log_info("gstreamer", "Query volume fraction: %f", volume);
	*v = volume;
	return 0;
}
static int output_gstreamer_set_volume(float value) {
	if (gm_alsa_volume_enabled()) return gm_alsa_volume_set(value);
	if (fixed_volume) return value == 1.0f ? 0 : -1;
	Log_info("gstreamer", "Set volume fraction to %f", value);
	g_object_set(player_, "volume", (double) value, NULL);
	return 0;
}
static int output_gstreamer_get_mute(int *m) {
	if (gm_alsa_volume_enabled()) return gm_alsa_mute_get(m);
	if (fixed_volume) { *m = 0; return 0; }
	gboolean val;
	g_object_get(player_, "mute", &val, NULL);
	*m = val;
	return 0;
}
static int output_gstreamer_set_mute(int m) {
	if (gm_alsa_volume_enabled()) return gm_alsa_mute_set(m);
	if (fixed_volume) return m == 0 ? 0 : -1;
	Log_info("gstreamer", "Set mute to %s", m ? "on" : "off");
	g_object_set(player_, "mute", (gboolean) m, NULL);
	return 0;
}

static void prepare_next_stream(GstElement *obj, gpointer userdata) {
	(void)userdata;
	g_mutex_lock(&uri_lock_);
	if (g_atomic_int_get(&playback_intent_) != INTENT_STOPPED &&
	    gs_next_uri_ && !gs_pending_uri_) {
		gs_pending_uri_ = gs_next_uri_;
		gs_pending_meta_ = gs_next_meta_;
		gs_next_uri_ = NULL; gs_next_meta_ = NULL;
		Log_info("gstreamer", "Prefetch next URI %s", gs_pending_uri_);
		g_object_set(obj, "uri", gs_pending_uri_, NULL);
	}
	g_mutex_unlock(&uri_lock_);
}

static void configure_http_source(GstElement *playbin, GstElement *source, gpointer unused) {
	(void)playbin; (void)unused;
	GstElementFactory *factory = gst_element_get_factory(source);
	if (!factory || strcmp(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)), "souphttpsrc") != 0) return;
	GObjectClass *klass = G_OBJECT_GET_CLASS(source);
	if (g_object_class_find_property(klass, "retries"))
		g_object_set(source, "retries", http_retries, NULL);
	if (g_object_class_find_property(klass, "timeout"))
		g_object_set(source, "timeout", (guint)http_timeout, NULL);
}

static int output_gstreamer_init(void)
{
	GstBus *bus;

	SongMetaData_init(&song_meta_);
	const gboolean native = dsd_mode && !strcmp(dsd_mode, "native");
	const gboolean dop = dsd_mode && !strcmp(dsd_mode, "dop");
	if (dsd_mode && !native && !dop && strcmp(dsd_mode, "off")) {
		Log_error("gstreamer", "Invalid DSD mode: %s", dsd_mode); return 1;
	}
	if (http_retries < 0 || http_retries > 10 || http_timeout <= 0 || http_timeout > 300) {
		Log_error("gstreamer", "Invalid HTTP retry or timeout setting"); return 1;
	}
	if (native || dop) {
		guint major, minor, micro, nano;
		gst_version(&major, &minor, &micro, &nano);
		if (major < 1 || (major == 1 && minor < 24)) {
			Log_error("gstreamer", "DSD requires GStreamer >= 1.24"); return 1;
		}
		GstElementFactory *parser = gst_element_factory_find("dffparse");
		if (!parser) { Log_error("gstreamer", "DSD requires dffparse (see installation guide)"); return 1; }
		gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(parser), GST_RANK_PRIMARY + 100);
		gst_object_unref(parser);
		/* Disallow the common libav DSD-to-PCM decoder paths. */
		const char *decoders[] = {"avdec_dsd_lsbf", "avdec_dsd_msbf", "avdec_dsd_lsbf_planar", "avdec_dsd_msbf_planar", NULL};
		for (int i = 0; decoders[i]; ++i) {
			GstPluginFeature *f = gst_registry_lookup_feature(gst_registry_get(), decoders[i]);
			if (f) { gst_plugin_feature_set_rank(f, GST_RANK_NONE); gst_object_unref(f); }
		}
		fixed_volume = TRUE;
	}
	if (fixed_volume && (!audio_sink || strcmp(audio_sink, "alsasink") || !audio_device || strncmp(audio_device, "hw:", 3) || audio_pipe || initial_db != 0.0)) {
		Log_error("gstreamer", "Fixed/DSD mode requires explicit alsasink, hw: device and unity gain"); return 1;
	}
	if (alsa_mixer_name && *alsa_mixer_name) {
		if (!fixed_volume || !audio_device ||
		    gm_alsa_volume_init(audio_device, alsa_mixer_name)) {
			Log_error("gstreamer", "ALSA mixer control requires fixed volume and a valid hw: mixer");
			return 1;
		}
	}
	scan_mime_list();

#if (GST_VERSION_MAJOR < 1)
	const char player_element_name[] = "playbin2";
#else
	const char player_element_name[] = "playbin";
#endif

	player_ = gst_element_factory_make(player_element_name, "play");
	if (!player_) {
		Log_error("gstreamer", "Can not initialize '%s'; "
			  "gstreamer installation with plugins complete ?",
			  player_element_name);
		return 1;
	}

        /* set buffer size */
        if (buffer_duration > 0) {
                gint64 buffer_duration_ns = round(buffer_duration * 1.0e9);
                Log_info("gstreamer",
                         "Setting buffer duration to %" PRId64 "ms",
                         buffer_duration_ns / 1000000);
                g_object_set(G_OBJECT(player_),
                             "buffer-duration",
                             buffer_duration_ns,
                             NULL);
        } else {
                Log_info("gstreamer",
			 "Buffering disabled (--gstout-buffer-duration)");
        }

	if (fixed_volume) {
		/* AUDIO | NATIVE_AUDIO: do not insert volume, resample or convert. */
		g_object_set(player_, "flags", 0x02 | 0x20 | (buffer_duration > 0 ? 0x100 : 0), NULL);
	}
	g_signal_connect(player_, "source-setup", G_CALLBACK(configure_http_source), NULL);
	bus = gst_pipeline_get_bus(GST_PIPELINE(player_));
	gst_bus_add_watch(bus, my_bus_callback, NULL);
	gst_object_unref(bus);

	if (audio_sink != NULL && audio_pipe != NULL) {
		Log_error("gstreamer", "--gstout-audosink and --gstout-audiopipe are mutually exclusive.");
		return 1;
	}
	if (video_sink != NULL && video_pipe != NULL) {
		Log_error("gstreamer", "--gstout-videosink and --gstout-videopipe are mutually exclusive.");
		return 1;
	}

	if (audio_sink != NULL) {
		GstElement *sink = NULL;
		Log_info("gstreamer", "Setting audio sink to %s; device=%s\n",
			 audio_sink, audio_device ? audio_device : "");
		sink = gst_element_factory_make (audio_sink, "sink");
		if (sink == NULL) {
		  Log_error("gstreamer", "Couldn't create sink '%s'",
			    audio_sink);
		  return 1; /* Never silently fall back to a different output. */
		} else {
		  if (audio_device != NULL) {
		    g_object_set (G_OBJECT(sink), "device", audio_device, NULL);
		  }
		  if (native || dop) {
		    GstElement *bin = gm_dsd_sink_new(sink, dop);
		    if (!bin) { Log_error("gstreamer", "Cannot construct DSD output"); return 1; }
		    gst_object_ref_sink(bin);
		    g_object_set(player_, "audio-sink", bin, NULL);
		    gst_object_unref(bin);
		  } else {
		    g_object_set (G_OBJECT (player_), "audio-sink", sink, NULL);
		  }
		}
	}
	if (audio_pipe != NULL) {
		GstElement *sink = NULL;
		Log_info("gstreamer", "Setting audio sink-pipeline to %s\n",audio_pipe);
		sink = gst_parse_bin_from_description(audio_pipe, TRUE, NULL);

		if (sink == NULL) {
			Log_error("gstreamer", "Could not create audio pipeline.");
			return 1;
		} else {
			g_object_set (G_OBJECT (player_), "audio-sink", sink, NULL);
		}
	}
	if (video_sink != NULL) {
		GstElement *sink = NULL;
		Log_info("gstreamer", "Setting video sink to %s", video_sink);
		sink = gst_element_factory_make (video_sink, "sink");
		g_object_set (G_OBJECT (player_), "video-sink", sink, NULL);
	}
	if (video_pipe != NULL) {
		GstElement *sink = NULL;
		Log_info("gstreamer", "Setting video sink-pipeline to %s\n", video_pipe);
		sink = gst_parse_bin_from_description(video_pipe, TRUE, NULL);

		if (sink == NULL) {
			Log_error("gstreamer", "Could not create pipeline.");
		} else {
			g_object_set (G_OBJECT (player_), "video-sink", sink, NULL);
		}
	}

	if (gst_element_set_state(player_, GST_STATE_READY) ==
	    GST_STATE_CHANGE_FAILURE) {
		Log_error("gstreamer", "Error: pipeline doesn't become ready.");
		return 1;
	}

	if (native || dop) {
		GstElement *sink = NULL;
		g_object_get(player_, "audio-sink", &sink, NULL);
		/* Open the real ALSA device before querying its caps; a closed
		 * alsasink only reports the broad pad template. */
		gboolean ready = sink && gst_element_set_state(sink, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE;
		if (ready) ready = gst_element_get_state(sink, NULL, NULL, 3 * GST_SECOND) == GST_STATE_CHANGE_SUCCESS;
		GstPad *pad = ready ? gst_element_get_static_pad(sink, "sink") : NULL;
		GstCaps *test = gst_caps_from_string("audio/x-dsd,format=DSDU8,layout=interleaved,reversed-bytes=false,rate=352800,channels=2");
		GstCaps *caps = pad ? gst_pad_query_caps(pad, test) : NULL;
		gboolean capable = caps && !gst_caps_is_empty(caps);
		if (caps) gst_caps_unref(caps);
		gst_caps_unref(test);
		if (pad) gst_object_unref(pad);
		if (sink) {
			gst_element_set_state(sink, GST_STATE_NULL);
			gst_object_unref(sink);
		}
		if (!capable) { Log_error("gstreamer", "Selected output does not advertise DSD64 path"); return 1; }
		register_mime_type("audio/dff");
		register_mime_type("audio/x-dff");
		Log_info("gstreamer", "Experimental DFF output: %s; GStreamer gain fixed", dsd_mode);
	}
	g_signal_connect(G_OBJECT(player_), "about-to-finish",
			 G_CALLBACK(prepare_next_stream), NULL);
	/* Preserve external ALSA mixer mute state on startup. */
	if (!gm_alsa_volume_enabled()) output_gstreamer_set_mute(0);
	if (initial_db < 0) {
		output_gstreamer_set_volume(exp(initial_db / 20 * log(10)));
	}

	return 0;
}

struct output_module gstreamer_output = {
        .shortname = "gst",
	.description = "GStreamer multimedia framework",
	.add_options = output_gstreamer_add_options,

	.init        = output_gstreamer_init,
	.set_uri     = output_gstreamer_set_uri,
	.set_next_uri= output_gstreamer_set_next_uri,
	.play        = output_gstreamer_play,
	.stop        = output_gstreamer_stop,
	.pause       = output_gstreamer_pause,
	.seek        = output_gstreamer_seek,

	.get_position = output_gstreamer_get_position,
	.get_volume  = output_gstreamer_get_volume,
	.set_volume  = output_gstreamer_set_volume,
	.get_mute  = output_gstreamer_get_mute,
	.set_mute  = output_gstreamer_set_mute,
};

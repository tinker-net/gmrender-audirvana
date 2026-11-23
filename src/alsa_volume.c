/* ALSA USB mixer control. Audio samples are never changed by this module.
 * The USB device may implement its mixer in hardware or firmware; this
 * interface does not by itself guarantee that output is bit-perfect.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <alsa/asoundlib.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "alsa_volume.h"
#include "logging.h"

static snd_mixer_t *mixer_;
static snd_mixer_elem_t *element_;
static long min_db_;
static long max_db_;

int gm_alsa_volume_enabled(void) {
    return mixer_ != NULL && element_ != NULL;
}

int gm_alsa_volume_init(const char *pcm_device, const char *control_name) {
    if (!pcm_device || strncmp(pcm_device, "hw:", 3) || !control_name || !*control_name)
        return -1;

    /* The PCM device contains ,DEV=0; a simple mixer needs only the card. */
    char *card = strdup(pcm_device);
    if (!card) return -1;
    char *suffix = strchr(card, ',');
    if (suffix) *suffix = '\0';

    snd_mixer_t *handle = NULL;
    int rc = snd_mixer_open(&handle, 0);
    if (rc >= 0) rc = snd_mixer_attach(handle, card);
    if (rc >= 0) rc = snd_mixer_selem_register(handle, NULL, NULL);
    if (rc >= 0) rc = snd_mixer_load(handle);
    if (rc < 0) {
        Log_error("alsa-volume", "Cannot open mixer on %s: %s", card, snd_strerror(rc));
        if (handle) snd_mixer_close(handle);
        free(card);
        return -1;
    }

    snd_mixer_elem_t *elem = NULL;
    for (snd_mixer_elem_t *e = snd_mixer_first_elem(handle); e; e = snd_mixer_elem_next(e)) {
        if (snd_mixer_elem_get_type(e) == SND_MIXER_ELEM_SIMPLE &&
            !strcmp(snd_mixer_selem_get_name(e), control_name)) {
            elem = e;
            break;
        }
    }

    if (!elem || !snd_mixer_selem_has_playback_volume(elem) ||
        snd_mixer_selem_get_playback_dB_range(elem, &min_db_, &max_db_) < 0 ||
        min_db_ >= max_db_) {
        Log_error("alsa-volume", "ALSA control '%s' not found or has no dB volume range on %s",
                  control_name, card);
        snd_mixer_close(handle);
        free(card);
        return -1;
    }

    /* A separate switch is required to implement UPnP SetMute reliably. */
    if (!snd_mixer_selem_has_playback_switch(elem)) {
        Log_error("alsa-volume", "ALSA control '%s' lacks a playback mute switch", control_name);
        snd_mixer_close(handle);
        free(card);
        return -1;
    }

    mixer_ = handle;
    element_ = elem;
    Log_info("alsa-volume", "Using mixer '%s' on %s (%.2f .. %.2f dB)",
             control_name, card, min_db_ / 100.0, max_db_ / 100.0);
    free(card);
    return 0;
}

int gm_alsa_volume_get(double *fraction) {
    if (!gm_alsa_volume_enabled() || !fraction) return -1;
    snd_mixer_handle_events(mixer_);
    long db;
    int rc = snd_mixer_selem_get_playback_dB(element_, SND_MIXER_SCHN_FRONT_LEFT, &db);
    if (rc < 0) return -1;
    *fraction = pow(10.0, db / 2000.0);
    return 0;
}

int gm_alsa_volume_set(double fraction) {
    if (!gm_alsa_volume_enabled() || !isfinite(fraction) ||
        fraction < 0.0 || fraction > 1.000001) return -1;

    /* ALSA dB values use hundredths of dB, UPnP/GStreamer use other units. */
    long requested = fraction <= 0 ? min_db_ : lround(2000.0 * log10(fraction));
    if (requested < min_db_) requested = min_db_;
    if (requested > max_db_) requested = max_db_;
    int rc = snd_mixer_selem_set_playback_dB_all(element_, requested, 0);
    if (rc < 0) Log_error("alsa-volume", "Set mixer volume failed: %s", snd_strerror(rc));
    return rc < 0 ? -1 : 0;
}

int gm_alsa_mute_get(int *muted) {
    if (!gm_alsa_volume_enabled() || !muted) return -1;
    snd_mixer_handle_events(mixer_);
    int on = 0;
    int rc = snd_mixer_selem_get_playback_switch(element_, SND_MIXER_SCHN_FRONT_LEFT, &on);
    if (rc < 0) return -1;
    *muted = !on;
    return 0;
}

int gm_alsa_mute_set(int muted) {
    if (!gm_alsa_volume_enabled()) return -1;
    int rc = snd_mixer_selem_set_playback_switch_all(element_, muted ? 0 : 1);
    if (rc < 0) Log_error("alsa-volume", "Set mixer mute failed: %s", snd_strerror(rc));
    return rc < 0 ? -1 : 0;
}

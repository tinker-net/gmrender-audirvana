/* ALSA simple-mixer volume control independent of the GStreamer audio path.
 * Only used when explicitly configured with --gstout-alsa-mixer-control.
 */
#ifndef GM_ALSA_VOLUME_H
#define GM_ALSA_VOLUME_H

int gm_alsa_volume_init(const char *pcm_device, const char *control_name);
int gm_alsa_volume_get(double *fraction);
int gm_alsa_volume_set(double fraction);
int gm_alsa_mute_get(int *muted);
int gm_alsa_mute_set(int muted);
int gm_alsa_volume_enabled(void);

#endif

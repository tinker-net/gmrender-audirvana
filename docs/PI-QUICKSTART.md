# Raspberry Pi / DietPi — reversible preview

This guide installs **gmrender-audirvana alongside the existing gmediarender**.
It does not replace the old binary, configuration or systemd unit.

## Important distinction

| | Original renderer | gmrender-audirvana |
|---|---|---|
| Files | Left in their existing location | `/opt/gmrender-audirvana` |
| Service | `gmediarender.service` (default; configurable) | `gmrender-audirvana.service` |
| Systemd configuration | Unmodified | New independent unit |
| Enabled at boot | Unmodified | Not enabled by installer |
| DAC | Same hardware, but only one process should open it at a time | Same hardware |

The installer uses `apt` to add build/runtime prerequisites. This does
not overwrite the original application files, but system packages and shared
libraries can be updated by the distribution's dependency resolution.

The switcher assumes the existing renderer is run by a systemd service.
If the original runs manually or under another service name, identify it
before switching; do **not** assume `gmediarender.service` exists.

## Install once (Debian / DietPi with GStreamer >= 1.24 for DSD)

```sh
git clone -b preview-0.2 https://github.com/tinker-net/gmrender-audirvana.git
cd gmrender-audirvana
sh scripts/install-pi.sh "hw:CARD=D10s,DEV=0" native
```

You can replace the ALSA device name using `aplay -l`. To use the experimental
DoP output, select `dop` instead of `native`. To omit DSD, select `off`.

The installer prepares a new unprivileged service but **does not stop or
enable/disable the original service**. The new service is not started yet.

## Optional ALSA volume control (Topping D10s)

The native DSD mode keeps GStreamer processing at unity. By default, UPnP
volume requests other than 100% are refused (UPnP 601) because changing
GStreamer volume could corrupt native DSD data. You can explicitly route
UPnP volume and mute to the DAC's ALSA mixer instead:

```sh
# Example for the D10s; the trailing space is part of the control name:
amixer -c D10s sget 'D10s '
printf 'GM_ALSA_MIXER_CONTROL="D10s "\\n' >> /etc/default/gmrender-audirvana
```

The systemd service passes this configuration through
`--gstout-alsa-mixer-control`. Re-run the installer to update the service
unit if it predates ALSA volume support, then switch to the preview.

The renderer reads the actual ALSA volume and mute state at startup. It
**does not** automatically turn up the DAC's existing volume. Setting 100%
in Audirvana may turn the DAC up to 0 dB, so reduce the analog playback
volume before testing.

This controls the USB device's ALSA mixer, not GStreamer's PCM samples.
**A USB mixer exposed by ALSA is not automatically an analog hardware
attenuator, nor does its presence guarantee bit-perfect output at
attenuated levels or volume support with native DSD.** Test both on your
particular DAC. Leave `GM_ALSA_MIXER_CONTROL` unset to preserve the
fixed-volume path and use external volume control.

## Switch in one command

```sh
/opt/gmrender-audirvana/bin/gmrender-switch status

# Test the new renderer:
sudo /opt/gmrender-audirvana/bin/gmrender-switch new

# Revert at any time:
sudo /opt/gmrender-audirvana/bin/gmrender-switch original
```

Switching releases the DAC and starts the other service. If the preview
fails at startup, the switcher attempts to restore the original automatically.

If your existing service has a different name, pass it as the final argument:

```sh
systemctl list-unit-files --type=service | grep -i 'gmrender'
sudo /opt/gmrender-audirvana/bin/gmrender-switch new my-existing-renderer.service
sudo /opt/gmrender-audirvana/bin/gmrender-switch original my-existing-renderer.service
```

These commands **do not change** the service's boot-time enabled/disabled
setting. On reboot, the existing enabled renderer will start normally;
the preview does not auto-start unless you enable it separately.

## Logs and troubleshooting

```sh
sudo journalctl -u gmrender-audirvana -f
cat /proc/asound/card*/pcm*p/sub*/hw_params
```

In Audirvāna select the zone **gmrender-audirvana**. Test PCM first with
an external amplifier at low volume. In native or DoP modes, digital gain is
fixed at unity: adjust listening level using your amplifier, not Audirvāna.

If the original is not managed by systemd, stop its process yourself before
opening the DAC with the new renderer. No systemd switch can reliably control
a renderer it does not own.

## Current verification scope

Automated CI covers compilation, SOAP/control responses, PCM test playback,
URI transitions, HTTP error reporting and simulated native DSD/DoP paths
(including DFF over HTTP with Range support). **It has not verified real
Audirvāna sessions or real Topping D10s/E30 II Lite DSD output.**

This is a preview, not a claim of perfect bit-perfect performance or
complete recovery from arbitrary network failures.

See `scripts/install-pi.sh` and `scripts/switch-renderer.sh` for the exact
changes made on the Raspberry Pi.

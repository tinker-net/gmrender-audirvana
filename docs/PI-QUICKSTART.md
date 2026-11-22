# Raspberry Pi / DietPi — preview 0.2

A headless UPnP/AV renderer for Audirvana. One gmediarender process; no MPD
and no OpenHome. This preview includes native DFF/DSD and DoP, clocked PCM
track transitions, HTTP error reporting and bounded source retries.
It is still an interoperability test build, not a certified final release.

## Install everything together

Native DSD and DoP require GStreamer 1.24 or later (Debian Trixie recommended).
The installer builds the renderer and the pinned LGPL DFF parser, installs
them under /opt/gmrender-audirvana and creates a separate systemd service
running as an unprivileged audio user. It does not replace the distribution's
gmediarender or stop it. It does not enable the new service at boot.

```sh
sudo apt-get install -y git
git clone --branch preview-0.2 https://github.com/tinker-net/gmrender-audirvana.git
cd gmrender-audirvana
sh scripts/install-pi.sh hw:CARD=D10s,DEV=0 native
sudo systemctl stop gmediarender
sudo systemctl start gmrender-audirvana
sudo journalctl -u gmrender-audirvana -f
```

For a different device, run `aplay -l` and use its card name. The default
configuration is intended for the Topping D10s. The installed renderer is
named gmrender-audirvana and uses a persistent UUID and port 49500.

## Switching DSD output

Edit `sudo nano /etc/default/gmrender-audirvana`. The supported values are:

- GM_DSD_MODE=native — DFF to native ALSA DSD. No PCM conversion.
- GM_DSD_MODE=dop — DFF to correctly framed DoP PCM carrier.
- GM_DSD_MODE=off — PCM-only advertisement.

Use `sudo systemctl restart gmrender-audirvana` after a change. Retain the
same UUID. Only use DoP with a DAC known to support DoP at the requested
rate; carrier detection is a property of hardware, not ALSA PCM caps.
Start with low external amplifier volume. This preview intentionally fixes
digital volume to unity and does not digitally mute or attenuate DSD or DoP.
Use the DAC/amplifier volume, not Audirvana's software volume. The strict
native PCM path will reject formats that the DAC cannot accept directly.
The optional DFF parser is installed at a pinned commit and lives under
the preview's private plugin directory, not in the system GStreamer registry.

## Verification with Audirvana

Select the gmrender-audirvana zone. In the device information, check for
`http-get:*:audio/dff:*` and WAV support. Keep software volume at 100%.
Try a PCM album, DSD64 and DSD128, then higher DSD rates only if your DAC
supports them. Test both native and Universal Gapless settings. Verify
position, metadata, pause, seek, next track, natural end, and history.

While playing, collect actual ALSA format and rate:

```sh
cat /proc/asound/card*/pcm*p/sub*/hw_params
```

Native DSD must show a DSD_U* format (e.g. DSD_U32_BE); for DoP it must
show a PCM carrier at the appropriate rate (DSD64 -> 176400 Hz), and the
DAC should indicate DSD. PCM frequency and sample representation must
match the requested source; no software resampler is inserted in fixed mode.

```sh
sudo journalctl -u gmrender-audirvana -b --no-pager > ~/gmrender-audirvana.log
```

The preview is compiled with SOAP action logging. Logs may contain URLs with
credentials or signed query parameters, so redact them before sharing.

## Rollback

```sh
sudo systemctl stop gmrender-audirvana
sudo systemctl start gmediarender
```

## Test scope and limitations

CI uses simulated sinks, deterministic synthetic DFF and PCM files, and
SOAP/HTTP regression tests. It does not have a physical USB DAC or a copy
of Audirvana. Relevant remaining validation includes negotiation on actual DACs,
DSD gapless, Universal Gapless streaming, and HTTP interruptions that cannot be
resumed with a byte range. The renderer does not falsely claim successful
software gain changes in fixed mode. Hardware mixer integration is not part
of this preview. Only stereo uncompressed DFF is explicitly announced; DST
and DSF as a direct UPnP input are not asserted. Unaligned DSD words are
rejected rather than silently truncating samples. The parser is a separate
LGPL plugin from dv1/gst-dsd-media-parsers; its source and license are
available at https://github.com/dv1/gst-dsd-media-parsers/tree/896c795abe568c0549d7e42fdae2876f96cef3d7.

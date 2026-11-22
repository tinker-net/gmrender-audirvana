# Raspberry Pi preview (Debian / DietPi)

This is an interoperability preview, not a finished release. It retains the original
GStreamer renderer and does not use MPD or OpenHome.

## Installation

Run as your normal login user. The installation uses /opt/gmrender-audirvana,
not /usr/bin or /usr/local/bin, and does not overwrite the packaged service.

```sh
sudo apt update
sudo apt install -y git build-essential autoconf automake libtool pkg-config \
  libglib2.0-dev libupnp-dev libgstreamer1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-alsa gstreamer1.0-tools
git clone --branch audirvana-compat https://github.com/tinker-net/gmrender-audirvana.git
cd gmrender-audirvana
./autogen.sh
./configure --prefix=/opt/gmrender-audirvana CPPFLAGS=-DENABLE_ACTION_LOGGING
make -j2
sudo make install
/opt/gmrender-audirvana/bin/gmediarender --version
```

Install the optional bad/ugly/libav GStreamer plugins later if needed for
additional input formats. They are not required for the initial WAV/PCM tests.
Action logging is compiled in for this preview to diagnose actual Audirvana SOAP
requests and responses. Logs may contain private stream URLs; redact
credentials or signed query parameters before sharing.

## Start a reversible test

Use the same network and DAC as the existing renderer. Stop playback in
Audirvana and close its current audio zone before switching.

```sh
aplay -l
mkdir -p "$HOME/.config/gmrender-audirvana"
test -s "$HOME/.config/gmrender-audirvana/uuid" || \
  cat /proc/sys/kernel/random/uuid > "$HOME/.config/gmrender-audirvana/uuid"
sudo systemctl stop gmediarender
sudo systemd-run --unit=gmrender-audirvana-preview --property=User="$(id -un)" \
  --property=SupplementaryGroups=audio --property=NoNewPrivileges=yes \
  /opt/gmrender-audirvana/bin/gmediarender \
  --friendly-name="gmrender-audirvana" \
  --uuid="$(cat "$HOME/.config/gmrender-audirvana/uuid")" --port=49500 \
  --gstout-audiosink=alsasink --gstout-audiodevice=hw:CARD=D10s,DEV=0 \
  --gstout-initial-volume-db=0 --logfile=stdout
```

Replace the ALSA device with the actual card name for other DACs. If another
renderer is using the DAC, stop its playback as well. Do not enable the new
service permanently yet. In Audirvana, select the separately named
`gmrender-audirvana` zone. Start with PCM, at a safe external amplifier volume.
Use 100% digital volume for the unity-gain test. Do not test DSD or DoP until
that path is specifically verified.

## Logs and verification

```sh
sudo journalctl -u gmrender-audirvana-preview -f
```

In another terminal, while playing:

```sh
cat /proc/asound/card*/pcm*p/sub*/hw_params
sudo journalctl -u gmrender-audirvana-preview -b --no-pager > "$HOME/gmrender-audirvana.log"
```

Record the Audirvana version, GStreamer version, whether Universal Gapless was
enabled, track format, and approximate time of any fault. Test a PCM album
first with the existing Audirvana settings; then change only Universal Gapless
for the second run. Try pause/resume, seek, skip, stop and a natural album end.

## Return to the original renderer

```sh
sudo systemctl stop gmrender-audirvana-preview
sudo systemctl start gmediarender
```

The original service file, binary and configuration remain unchanged.
The preview does not start automatically at boot.

## Known limitations

- The automated tests exercise UPnP/SOAP on a fake audio sink, not a real DAC
  or Audirvana.
- Robust native gapless transitions, transparent HTTP resume, native DSD,
  DoP, and verified bit-perfect playback are still work in progress.
- For DSD, Audirvana requires a renderer to announce audio/dff explicitly.
  Do not force that MIME type until a DFF-to-native-DSD output path has
  been validated. GStreamer 1.24+ has native DSD infrastructure but this
  alone does not guarantee that the installed DFF parser and playback
  pipeline preserve DSD samples.
- Test files and debug logs are not required at runtime.

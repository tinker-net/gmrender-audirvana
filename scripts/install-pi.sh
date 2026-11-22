#!/bin/sh
# Install alongside, not over, the distribution's gmediarender.
# Usage: sh scripts/install-pi.sh [hw:CARD=D10s,DEV=0] [native|dop|off]
set -eu
device=${1:-hw:CARD=D10s,DEV=0}
mode=${2:-native}
prefix=/opt/gmrender-audirvana
case "$device" in hw:*) ;; *) echo "Select a direct hw: ALSA device" >&2; exit 1;; esac
case "$device" in *[!a-zA-Z0-9_:=,.-]*) echo "Invalid device name" >&2; exit 1;; esac
case "$mode" in native|dop|off) ;; *) echo "Use native, dop or off" >&2; exit 1;; esac
cd "$(dirname "$0")/.."
sudo apt-get update
sudo apt-get install -y git build-essential autoconf automake libtool pkg-config \
  libglib2.0-dev libupnp-dev libgstreamer1.0-dev \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good gstreamer1.0-alsa \
  gstreamer1.0-tools alsa-utils
if [ "$mode" != off ]; then
  sudo apt-get install -y libgstreamer-plugins-base1.0-dev meson ninja-build g++
  pkg-config --atleast-version=1.24 gstreamer-audio-1.0 || {
    echo "Native/DoP mode requires GStreamer 1.24 or newer (Debian Trixie recommended)." >&2
    exit 1
  }
fi
./autogen.sh
./configure --prefix="$prefix" CPPFLAGS=-DENABLE_ACTION_LOGGING
make -j2
sudo systemctl stop gmrender-audirvana.service 2>/dev/null || true
sudo make install
if [ "$mode" != off ]; then
  sh scripts/install-dsd-parser.sh "$prefix"
fi
sudo install -d -m 755 /etc/gmrender-audirvana
if ! sudo test -s /etc/gmrender-audirvana/uuid; then
  cat /proc/sys/kernel/random/uuid | sudo tee /etc/gmrender-audirvana/uuid >/dev/null
fi
uuid=$(sudo cat /etc/gmrender-audirvana/uuid)
if ! sudo test -e /etc/default/gmrender-audirvana; then
  printf 'GM_DEVICE=%s\nGM_DSD_MODE=%s\nGM_UUID=%s\n' "$device" "$mode" "$uuid" | sudo tee /etc/default/gmrender-audirvana >/dev/null
else
  echo "Existing /etc/default/gmrender-audirvana retained; check device and mode."
fi
# A dedicated unprivileged system account, separate from the distro service.
if ! id gmrender-audirvana >/dev/null 2>&1; then
  sudo useradd --system --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin --gid audio gmrender-audirvana
fi
sudo tee /etc/systemd/system/gmrender-audirvana.service >/dev/null <<'UNIT'
[Unit]
Description=gmrender-audirvana UPnP/AV renderer
After=network-online.target sound.target
Wants=network-online.target

[Service]
Type=simple
User=gmrender-audirvana
Group=audio
EnvironmentFile=/etc/default/gmrender-audirvana
Environment=GST_PLUGIN_PATH=/opt/gmrender-audirvana/lib/gstreamer-1.0
Environment=GST_REGISTRY=/run/gmrender-audirvana/registry.bin
RuntimeDirectory=gmrender-audirvana
ExecStart=/opt/gmrender-audirvana/bin/gmediarender --friendly-name=gmrender-audirvana --port=49500 --uuid=${GM_UUID} --gstout-audiosink=alsasink --gstout-audiodevice=${GM_DEVICE} --gstout-dsd-mode=${GM_DSD_MODE} --gstout-fixed-volume --gstout-initial-volume-db=0 --gstout-http-retries=3 --gstout-http-timeout=15 --logfile=stdout
NoNewPrivileges=true
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
UNIT
sudo systemctl daemon-reload
echo "Installed. Original gmediarender was not changed or stopped."
echo "Adjust amplifier to a low level before testing, especially DoP."
echo "Start: sudo systemctl stop gmediarender && sudo systemctl start gmrender-audirvana"
echo "Logs:  sudo journalctl -u gmrender-audirvana -f"
echo "Undo:  sudo systemctl stop gmrender-audirvana && sudo systemctl start gmediarender"

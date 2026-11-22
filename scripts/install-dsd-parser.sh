#!/bin/sh
# Build a pinned LGPL GStreamer DFF/DSF parser without replacing system plugins.
set -eu
prefix=${1:-/opt/gmrender-audirvana}
revision=896c795abe568c0549d7e42fdae2876f96cef3d7
case "$prefix" in /*) ;; *) echo "Use an absolute prefix" >&2; exit 1;; esac
pkg-config --atleast-version=1.24 gstreamer-audio-1.0 || {
  echo "DSD requires GStreamer >= 1.24 and libgstreamer-plugins-base1.0-dev" >&2
  exit 1
}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT HUP INT TERM
git init -q "$work/parser"
git -C "$work/parser" fetch --depth=1 https://github.com/dv1/gst-dsd-media-parsers.git "$revision"
git -C "$work/parser" checkout --detach FETCH_HEAD
test "$(git -C "$work/parser" rev-parse HEAD)" = "$revision"
meson setup "$work/build" "$work/parser" --buildtype=release --prefix="$prefix" --libdir=lib
meson compile -C "$work/build" -j2
plugin="$work/build/libgstdsdmediaparsers.so"
test -f "$plugin"
if [ -d "$prefix" ] && [ -w "$prefix" ]; then
  install -d "$prefix/lib/gstreamer-1.0" "$prefix/share/doc/gst-dsd-media-parsers"
  install -m 755 "$plugin" "$prefix/lib/gstreamer-1.0/"
  install -m 644 "$work/parser/COPYING" "$prefix/share/doc/gst-dsd-media-parsers/"
else
  sudo install -d "$prefix/lib/gstreamer-1.0" "$prefix/share/doc/gst-dsd-media-parsers"
  sudo install -m 755 "$plugin" "$prefix/lib/gstreamer-1.0/"
  sudo install -m 644 "$work/parser/COPYING" "$prefix/share/doc/gst-dsd-media-parsers/"
fi
GST_PLUGIN_PATH="$prefix/lib/gstreamer-1.0" gst-inspect-1.0 dffparse >/dev/null
echo "DFF parser installed. Set GST_PLUGIN_PATH=$prefix/lib/gstreamer-1.0 for the renderer."

#!/bin/sh
# Switch between two independently installed systemd renderers.
# The original service is never uninstalled, reconfigured, enabled or disabled.
# Usage (as root or using sudo):
#   gmrender-switch status [original.service]
#   gmrender-switch new [original.service]
#   gmrender-switch original [original.service]
set -eu

command=${1:-status}
original=${2:-gmediarender.service}
preview=gmrender-audirvana.service

case "$original" in
  ""|*[!a-zA-Z0-9_.@-]*)
    echo "Invalid original systemd service name" >&2; exit 2;;
esac

if [ "$original" = "$preview" ]; then
  echo "Original and preview service must be different" >&2
  exit 2
fi

status() {
  for unit in "$original" "$preview"; do
    if systemctl cat "$unit" >/dev/null 2>&1; then
      state=$(systemctl is-active "$unit" 2>/dev/null || true)
      enabled=$(systemctl is-enabled "$unit" 2>/dev/null || true)
      printf '%s: %s (enabled: %s)\n' "$unit" "$state" "$enabled"
    else
      printf '%s: not installed as a systemd service\n' "$unit"
    fi
  done
}

if [ "$command" = status ]; then
  status
  exit 0
fi

case "$command" in new|original) ;; *)
  echo "Usage: $0 {status|new|original} [original.service]" >&2
  exit 2;;
esac

if [ "$(id -u)" -ne 0 ]; then
  echo "Run the switching command with sudo" >&2
  exit 2
fi

if ! systemctl cat "$original" >/dev/null 2>&1; then
  echo "Cannot find '$original'. Pass the actual name of your original systemd unit." >&2
  exit 1
fi
if ! systemctl cat "$preview" >/dev/null 2>&1; then
  echo "Cannot find '$preview'. Install the preview first." >&2
  exit 1
fi

if [ "$command" = new ]; then
  # Release the DAC before opening the preview; do not touch the
  # original unit file or its boot-time enablement.
  systemctl stop "$original"
  if systemctl start "$preview" && systemctl is-active --quiet "$preview"; then
    echo "Preview active; original installation untouched."
  else
    echo "Preview failed. Restoring the original renderer..." >&2
    systemctl stop "$preview" 2>/dev/null || true
    systemctl start "$original" || true
    status
    exit 1
  fi
else
  systemctl stop "$preview"
  systemctl start "$original"
  echo "Original renderer restored; preview remains installed but stopped."
fi
status

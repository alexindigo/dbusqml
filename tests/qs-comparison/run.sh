#!/usr/bin/env bash
set -euo pipefail

# Run the Quickshell reactive-binding comparison spike inside the
# dbusqml devcontainer. Assumes image is already built as
# dbusqml-dev:local (via `docker build .devcontainer/ -t dbusqml-dev:local`).

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
IMAGE="dbusqml-dev:local"

if ! docker image inspect "$IMAGE" &>/dev/null; then
  echo "Building $IMAGE..." >&2
  docker build -t "$IMAGE" "$REPO_ROOT/.devcontainer" >&2
fi

echo "=== Container QS version ===" >&2
docker run --rm "$IMAGE" pacman -Qi quickshell | grep -E '^(Name|Version)' >&2

echo "=== Verifying host DBus visibility from container ===" >&2
docker run --rm \
  --network=host \
  -v /run/dbus:/run/dbus \
  -e DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket \
  "$IMAGE" \
  gdbus introspect --system \
    --dest org.freedesktop.NetworkManager \
    --object-path /org/freedesktop/NetworkManager \
  | grep -E 'WirelessEnabled|<node ' | head -5 >&2

echo "=== Running spike (4s timeout) ===" >&2
SPIKE_TMP="$(mktemp -d "${TMPDIR:-/tmp}/dbusqml-qs.XXXXXX")"
trap 'rm -rf "$SPIKE_TMP"' EXIT
docker run --rm \
  --network=host \
  -v /run/dbus:/run/dbus \
  -v "$REPO_ROOT:/workspaces/dbusqml" \
  -v "$SPIKE_TMP:$SPIKE_TMP" \
  -w /workspaces/dbusqml \
  -e DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket \
  -e QT_FORCE_STDERR_LOGGING=1 \
  -e TMPDIR="$SPIKE_TMP" \
  -e XDG_RUNTIME_DIR="$SPIKE_TMP/qs-runtime" \
  -e QT_QPA_PLATFORM=offscreen \
  "$IMAGE" \
  bash -c 'mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR" && timeout 4 quickshell -p tests/qs-comparison/shell.qml 2>&1 || true'

echo "=== Done ===" >&2

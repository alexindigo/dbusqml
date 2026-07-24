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
docker run --rm \
  --network=host \
  -v /run/dbus:/run/dbus \
  -v "$REPO_ROOT:/workspaces/dbusqml" \
  -w /workspaces/dbusqml \
  -e DBUS_SYSTEM_BUS_ADDRESS=unix:path=/run/dbus/system_bus_socket \
  -e QT_FORCE_STDERR_LOGGING=1 \
  -e XDG_RUNTIME_DIR=/tmp/qs-runtime \
  -e QT_QPA_PLATFORM=offscreen \
  "$IMAGE" \
  bash -c 'mkdir -p /tmp/qs-runtime && chmod 700 /tmp/qs-runtime && timeout 4 quickshell -p tests/qs-comparison/shell.qml 2>&1 || true'

echo "=== Done ===" >&2

#!/usr/bin/env bash
set -euo pipefail

# Qt6 tools (qmllint, qmlformat) live under /usr/lib/qt6/bin in the
# devcontainer image, not on the default PATH.
export PATH="/usr/lib/qt6/bin:$PATH"

# Runs inside devcontainer. Called by .githooks/pre-commit with no arguments.
# Lints the entire repo (not just staged files) — dbusqml is small and
# low-level enough that a full check per commit is warranted.

# -- QML --
mapfile -t QML < <(find . -name '*.qml' -not -path './build*/*' -not -path './.git/*')
if [ ${#QML[@]} -gt 0 ]; then
  qmllint "${QML[@]}"
  qmlformat -n "${QML[@]}"
fi

# -- C++ --
mapfile -t CXX < <(find . \( -name '*.cpp' -o -name '*.h' \) \
  -not -path './build*/*' -not -path './.git/*')
if [ ${#CXX[@]} -gt 0 ]; then
  clang-format --dry-run --Werror "${CXX[@]}"
fi

# -- Shell (scripts/*) --
mapfile -t SH < <(find scripts -type f 2>/dev/null || true)
if [ ${#SH[@]} -gt 0 ]; then
  shellcheck "${SH[@]}"
  shfmt -i 4 -d "${SH[@]}"
fi

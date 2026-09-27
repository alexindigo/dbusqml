#!/usr/bin/env bash
# tsan-crosscheck.sh — the independent cross-check of the F-amended gate
# (qwen's amendment): re-greps the classifier's dumped foreign blocks and
# requires ZERO of our artifact basenames in their ATTRIBUTION sections
# (access stacks + allocation/Location stacks; thread-creation stacks never
# attribute — owner ruling, question-1). Shares NO code with
# tsan-classify.py (deliberately: a classifier bug must not propagate).
#
# Usage: tsan-crosscheck.sh <foreign-blocks-file>
# Exit 0 = every foreign block clean; 1 = disagreement (or unreadable input).
set -uo pipefail

DUMP="${1:-}"
if [ -z "$DUMP" ] || [ ! -f "$DUMP" ]; then
    echo "tsan-crosscheck: usage: tsan-crosscheck.sh <foreign-blocks-file>" >&2
    exit 2
fi

# Minimal independent block walker (awk): within each block, drop sections
# whose header says "created by" (thread-creation stacks), keep everything
# else, then grep the kept text for our artifact basenames.
# The 'dbusqml' alternative must name a SOURCE FILE (…/dbusqml/….cpp:NN —
# how our frames print), never the bare directory name: on CI the workspace
# itself is '/home/runner/work/dbusqml/', so module paths like
# '…/work/dbusqml/Qt/6.8.2/…/libQt6DBus.so.6' in SUMMARY lines would
# false-positive a substring match (the release-ceremony CI red, layer 3).
hits=$(awk '
  /^### foreign block from / { inblock=1; increation=0; next }
  inblock && /^WARNING: ThreadSanitizer/ { increation=0 }
  inblock && /^[[:space:]]*$/ { increation=0; next }
  inblock && /created by/ { increation=1; next }
  inblock && !increation { print }
' "$DUMP" | grep -cE 'libdbusqml|dbusqml[^ )]*\.(cpp|h|qml)|test_|fuzz_|tsan_canary' || true)

if [ "$hits" != "0" ]; then
    echo "tsan-crosscheck: DISAGREEMENT — $hits artifact-basename line(s) in" \
        "foreign-classified blocks' attribution sections" >&2
    exit 1
fi
echo "tsan-crosscheck: clean (zero artifact basenames in foreign blocks' attribution sections)"
exit 0

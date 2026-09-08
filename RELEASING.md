# Releasing dbusqml — technical gates only

The owner ceremony (push / tag / timeshift discipline / AUR publish /
pings) lives in plans-land, never here. This file is the mechanical
checklist the executor runs BEFORE handing a candidate to the ceremony.

## Gate 1 — full suite green on the VM

- `scripts/run-tests <build>` — all four ctest targets + QML suite,
  arch-niri VM, Debug + `BUILD_TEST_MODE=ON`.
- Sanitizer intent: the CI `sanitizers` job (ASan+LSan, TSan) covers
  what the VM cannot; do not tag with it red.

## Gate 2 — VM gate incl. consumer legs

- The plan's `gate/` harness re-run ×2 (VM reverted before and between).
- **Tag criterion for 0.9.1: the consumer gate's FileChooser cancel leg
  is green** (`gate/gate-run.sh` — `cancel-leg-settles`). A red cancel
  leg blocks the tag unconditionally (council-unanimous).

## Gate 3 — docs current

- No stale feature lists (README features == shipped surface), no
  invalid-QML examples (DESIGN), script names/dirs match `scripts/`,
  KNOWN_ISSUES pruned to the ≥6.8 floor, no future-work files in-repo
  (PARITY.md axiom). The V1 pass re-verifies every audit item ID
  against source.

## Gate 4 — dist/tag mechanics

- `cmake --install` from a clean prefix works; installed-prefix test
  rerun passes (xdp pattern — CI covers it).
- Version bump is the tip commit (`CMakeLists.txt` VERSION only),
  signed, monotone, real dates; push-time timeshift per the owner
  ruling on record (2026-09-07 = holiday = weekend).

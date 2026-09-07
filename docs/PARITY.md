# Parity discipline (design-time requirements)

Every future mechanism in dbusqml — resolver, walker, send site, lifecycle —
must satisfy the four checklists below BEFORE it ships. They are the
distilled form of the 0.9.x parity cycles (audit:
`~/Documents/dbusqml/plans/served-surface-v0.9.0/audit-2-findings.md`;
release report: `~/Documents/dbusqml/plans/fix-parity-v0.9.1/`), where each
class of defect recurred until a checklist made it impossible.

## 1. Resolver consumer-set completeness

Every name-resolution mechanism (alias lookup, fold lookup, wire-name
resolution, catalog merge) must enumerate ALL its consumers and behave
identically at each:

- List the consumers explicitly in the design (JS dispatch, C++ dispatch,
  XML generation, catalog in-args, GetAll routing, …).
- A lookup order fixed in one consumer (`matchedName` → exact → fold) must
  be the SHARED implementation, not a copy (the fold word-boundary defect
  lived in exactly one of two copies).
- New alias/fold behavior needs a test at EVERY consumer, not one.
- Precedent: A1 (alias JS path), A8/A13 (catalog in-args, `_signatures`
  keys), A14/D5 (shared fold impl).

## 2. Walker position parity

Every signature-walking mechanism (marshal, unmarshal, coerce, guard) must
behave identically at top level and at every nested position:

- Test matrix: same value × top-level/nested × read/write.
- A walker that recurses needs the depth cap (32) and loud failure —
  unbounded recursion is a hang, not a bug report.
- Declared signatures drive; inference is the fallback, never the silent
  override (D4: gadget passthrough only on signature equality).
- Unproducible shapes fail LOUD with the declared signature named — never a
  different wire type with no notice, never a libdbus abort.
- New metatype registrations follow the signature-slot rules
  (`dbussignatureslots.h`: assign-once, no marshall operators, validated
  before registration, mutex never held while streaming).
- Precedent: B2–B6 (nested av/ao, ay coercion, fd validity), F1/B9
  (signature-slot pool).

## 3. Guard/reply-flag parity at every send site

Every site that puts bytes on the bus (reply, signal relay, `emitSignal`,
held reply, error reply) must apply the SAME guards with the SAME flags:

- `wireMarshalable` before every send; send-return checked and logged.
- Declared types applied at emission; mismatched payloads warn + skip
  (signals) or warn + drop (declared-void replies) — never silently sent.
- Error names grammar-validated before `createErrorReply`.
- Library-mechanism names (`emitSignal`, `holdReply`, `unregister`) are
  never served: skipped by dispatch AND the XML, with the load warning true.
- A new send site copies the checklist, not the code — then gets its own
  test asserting warn+skip+service-alive (kill-class protocol:
  timeboxed-subprocess probes for kill-class values).
- Precedent: C0 (relay guard), A6/A10/D1 (emission types, skip lists),
  B7/B11 (invoke loud-fail, error-name validation).

## 4. Completion-gated lifecycle for every element

Every lifecycle mechanism (claim, registration, watch, teardown, pending
call) must gate each transition on OBSERVED completion, never on
wall-clock luck:

- Sync barriers on bus STATE (`serviceOwner`, owner-changed delivery),
  not on fixed sleeps. A `QTRY` window that passes 5/9 times is a
  roulette wheel, not a test — the recurring-flake lesson (takeover 4/9
  across two cycles, accepted twice) is that flakes are race EVIDENCE.
- A mutex must NEVER span a blocking bus call when the party needing the
  mutex also owns bus I/O progress (QtDBus's manager thread is the sole
  socket reader). Violation = deadly embrace, proven twice
  (`DBusPathDispatcher` detach AND attach paths).
- Teardown markers (tombstone/pending claims) for any unregister/register
  window a concurrent attach could adopt; ownership gating so lost names
  never take futile blocking round-trips.
- Stress shapes, not repetition counts: flood `NameOwnerChanged` while
  tearing down (deterministic embrace), concurrent same-name attach
  (registration loss), takeover ×20+. Re-running a timing-sensitive test
  re-rolls dice; a stress test with a wall-clock bound converts a hang
  into a loud failure.
- Debug-only invariant guards (thread-local "holding mutex" flags
  asserted clear before bus calls) turn future recurrences into
  deterministic failures.
- Precedent: F4 (dispatcher detach/attach embraces, D8 barriers, churn
  stress).

## Threading contract (attach/detach)

Attach/detach for a given (connection, service) are consumer-serialized —
in practice both run on the QML/main thread, which is the only
configuration the teardown markers protect. A C++ consumer driving
attach/detach for the SAME name from two threads concurrently is
unsupported: the registry mutex guards the manager thread, not concurrent
consumer threads. (Tracked hazard: `handleServiceOwnerChange` invokes
`nameAcquired`/`nameLost` on the manager thread — QPointer check-then-deref
vs main-thread deletion, and `Qt::DirectConnection` consumers would run JS
on QtDBus's thread. Recorded, not fixed: see the release report.)

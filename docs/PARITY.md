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
- NO_REPLY_EXPECTED is honored at every dbusqml send site (the code tags
  this B4). Under `captureSubtree`, dbusqml's mirrored fallback on a
  captured child honors it while Qt's own bottom fallback on a plain
  adaptor replies anyway — the one documented asymmetry between a captured
  child and a plain adaptor; pinned by the parity table's row 20.
- A method miss on a served interface answers `UnknownMethod` (Qt's text)
  at every dbusqml path, B4-guarded; only an interface no adaptor serves
  answers `UnknownInterface`.

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

## 5. Exactly-one-reply-per-serial (for-all-times Phase 0)

Every dispatch path (sync return, throw-after-hold, held settle,
teardown/unregister erroring) emits EXACTLY ONE reply per incoming
message serial — asserted at the wire through the raw-libdbus oracle
(`tests/wire-oracle/`). The oracle counts BOTH directions, keyed by the
spec's serial correlation: `ReplyCount` tallies replies the oracle
itself emits (its own service handlers); `ReceivedCount` tallies
replies the oracle receives as a caller (keyed by `reply_serial`) —
the direction that actually observes an adaptor under test, via
`CallAdaptor`. The held-reply pins in `test_dbusadaptor.cpp` drive their
calls THROUGH the oracle process and assert `ReceivedCount == 1` after
the settle + quiet window (and across teardown for the idempotent-hold
pin); the oracle is not Qt marshalling both ways, so vacuous passes die
structurally, and `testOracleSensitivityDoubleReply` (a deliberately
double-sending in-suite service MUST read back 2) proves the `== 1`
evidence is live:

- `holdReply()` registers the held object in the dispatch context; the
  error branch settles THAT object via `sendError`, never direct-replies
  (direct + unsettled-held = double-reply hazard).
- Non-Error throws after hold are normalized to Failed errors (no
  silent swallows, anywhere).
- Dispatch context is a STACK (RAII scope per invocation) — nested
  dispatches restore, never clobber, the outer call.
- Every `conn.send` result is checked (replies as C0 did signals).
- Held-path skips are loud (member named in the warning).
- Precedent: P0 (for-all-times Phase 0 fix + hold+throw matrix).

The referee counts remote replies of both kinds once each; doubles are
detected within a declared 500 ms quiet window after the expected count
(`oracleSettledCount`). Beyond that bound the gate is silent by
construction — an observation bound, not a proof of finality.

## 6. No emission from foreign threads

Ownership notifications (`nameAcquired`/`nameLost`) are delivered on the
main thread via the process-lifetime `OwnerChangeRelay` — no adaptor
pointer ever crosses a thread (features train, Phase 2). The queued
lambda records the claim transition on the attach thread and marshals a
value-only note; the relay re-resolves holders under the lock on the
main thread, drops the lock, then delivers. Never a `QPointer` check-then-deref on the
manager thread; queued `invokeMethod` on a raw pointer is NOT
teardown-safe (still check-then-post). Precedent: T1 (notifier corpse
067aa01, postEvent corpse t1-spike-findings.md F3, concilium-unanimous
candidate 4).

Under a captured prefix (`captureSubtree: true`), messages are delivered
to handlers in bus arrival order. A handler that spins a nested event
loop can reorder its successors — unsupported, same as today's contract
(no nested event loops in handlers).

## 7. No future-work files in-repo (D12)

Roadmaps live in plans-land. No surveyed prior-art repo keeps a
future-work file in-repo (quickshell: `changelog/next.md` accumulator
only). `FutureDevelopment.md` was removed; its one live item (caller
identification via `QDBusContext`) is scheduled in the 0.10 train.

## 8. KNOWN_ISSUES floor-pruning

Entries for Qt below the library floor (6.8) are pruned — history lives
in git. Resolved-history sections are pruned the same way; only live
entries affecting floor-or-newer Qt stay.

## 9b. Connection loss is reported, never reconnected (P5)

Loss is observed through failing calls (`QDBusError::Disconnected`), not
`org.freedesktop.DBus.Local.Disconnected` — QtDBus consumes the local
signal internally and never delivers it to match rules (fork-VM-proven).
One daemon-facing ping per connection-moment detects death; `connected`
flips + `disconnected()` fires once; proxies flip to `Error` and drop
match subscriptions; served claims emit `nameLost` through the T1 relay.
No resubscribe/reconnect, ever.

## 9c. Failed writes re-fetch; concurrent ops dedupe (R3/P9)

A rejected `Set` re-fetches the property from the service
(`Properties.Get` once the write chain settles) and the server's value
lands in the map — no local rollback inference; the proxy never invents
a value. `propertyWriteFailed` fires unchanged (KDE signal shape). A
failed re-fetch keeps the current value + warns loud. Duplicate
in-flight `Get` coalesces (one wire call, all waiters answered); `Set`
dedupe is latest-wins (interleaved set/set converges to the last value).

## 9. Flake policy

A flake that recurs is a defect with luck — root-cause it (the takeover
flake was a real deadlock, accepted twice). Timing-sensitive tests use
sync barriers on bus STATE, never fixed sleeps; stress shapes with
wall-clock bounds convert hangs into loud failures. TSan in CI catches
the thread class nobody had listed.

**TSan gate (F-amended, tsan-classifier-gate cycle — supersedes the
road-to-one C3 grep-gate).** Two layers, each with its own job:

- **Detector layer (recording).** The gate is a targeted selection of
  per-test processes (full-suite ctest wedges TSan's thread registry
  under churn geometries — L3 wedge triage: thread-count effect, not a
  program race), run with `tests/tsan-suppressions.txt` — typed
  (`race:`/`thread:`/`deadlock:`), symbol-or-module-named entries with
  per-entry evidence blocks (C1; hex offsets never match and do not
  port). `TSAN_OPTIONS=exitcode=0` throughout — the detector's process
  exit is never the verdict (the classifier owns it). The detector
  layer's known classes (all Qt/libdbus-internal; zero dbusqml frames as
  writer): thread-leak on `QThread::start` (per-test process teardown);
  two libdbus lock-order-inversions (`dbus_bus_register`,
  `dbus_connection_preallocate_send` path); the QtDBus bus-bind ALLOCATOR
  race (`qDBusBindToApplication` new-vs-delete vs the QDBusConnection
  worker); the proxy-teardown trio (`QObject::~QObject`,
  `QCoreApplication::removePostedEvents`,
  `QDBusServiceWatcher::setConnection`-side delete — all via
  `DBusProxy::~DBusProxy` caller context) racing the bus worker; the
  ledger-zero QML/QQmlThread churn classes. Its accepted-risk register:
  `0x2d1ab3` = `QArrayData::reallocateUnaligned` (main-thread realloc of
  an implicitly-shared container during meta-call argument churn vs the
  worker's memmove) — *accepted-unresolved-Qt-internal-risk, NOT a proven
  false positive*; falsifier: a TSan-instrumented Qt build (the B run,
  `~/Documents/dbusqml/todos/TODO.md`).
- **Verdict layer (classification).** `tests/tsan-classify.py` parses
  the captured per-test logs into complete TSan report blocks and rules
  on each:
  - any block with one of our artifacts (`libdbusqml`, the test/fuzzer/
    canary binaries) in an ATTRIBUTION stack (either access stack, or
    the allocation/Location stack) → **FAIL**. The attribution principle
    (owner ruling on question-1): **participation attributes; provenance
    doesn't.** Thread-CREATION stacks never attribute — Qt spawns its
    worker once per process from whatever code first touches the bus,
    which in this suite is our frames by construction; creation-stack
    module basenames are recorded per census line (drift visibility).
    (One type-scoped exception, `thread leak`, argued in the report-type
    coverage paragraph below.)
  - a block whose attribution stacks are all-foreign (Qt/libdbus/glibc/
    libtsan modules, TSan interceptors) AND whose fingerprint matches an
    entry in `tests/tsan-foreign-register.toml` under the THREE-layer
    match (fingerprint v3, tsan-gate-edgecases) → counted, censused,
    non-failing. The layers, all required:
    1. **Version**: the log's Qt version (QtTest config line) is in the
       entry's `qt_validated` list — else the entry is inert (below).
    2. **Arrangement** (v2 — shape): the ordered per-stack record list,
       one per attribution stack in appearance order: (role
       [`access-first`/`access-second`/`alloc`], frame-#0 interceptor
       name, first resolvable module basename below the interceptor, the
       acting thread's context [`main` or the worker's name parsed from
       the block's own creation header], access kind:size). A future,
       distinct Qt race that merely shares the pooled ingredients (same
       functions and libraries in a different arrangement) does NOT
       match — it fails the knob and earns its own entry.
    3. **Site anchors** (v3 — location) **+ binary identity** (v3.1,
       tsan-buildid-fold): the ordered list of `module+0xoffset@buildid`
       TRIPLES (the racing frame below each stack's interceptor), checked
       only against the exact validated Qt version they were captured on —
       module-relative offsets are ASLR-independent and stable per Qt
       build, and the version gate already pays the cross-version
       maintenance, so the old portability objection to offsets no longer
       applies. A version string is not a binary identity — the same
       "6.11.2" rebuilt differently must not match, so each anchor triple
       carries the module's BuildId (the content-hash identity printed on
       every frame line); a frame with no BuildId token cannot prove its
       binary identity → fail-closed → FAIL. Same arrangement + different
       site, same site + different build, or absent build identity: three
       distinct near-misses, the census names which, the gate FAILs. (A
       log with no parseable Qt version — probe binaries, the foreign
       canary — never gates layer 1, and matches by TRIPLE against ANY
       validated version's list: its binary must BE a validated build,
       proven by content hash, not assumed.)

       **The identity ladder is complete**: report type → arrangement
       (shape) → site (location) → binary identity (content hash). Beyond
       binary content identity, report content is run-variable (addresses,
       pids, timestamps) — structurally unmatchable. No further identity
       rung exists; the register's identity model is final by
       construction.
  - a foreign block with NO register match → **FAIL** (the knob, council
    5–1; registering is cheap — the hitter adds the evidence-blocked
    entry in the same change).
  - any ambiguity (truncated/unparseable block, module-less frame in an
    attribution stack, zero blocks with a WARNING present or a nonzero
    recorded detector exit) → **FAIL**. Also fail-closed-anomaly: a
    WARNING of an unknown/future report type, a `==N==ERROR:
    ThreadSanitizer:` fatal-signal report (never foreign, never PASS), an
    orphan SUMMARY line, and any register-schema malformation at load
    (unknown keys, missing required fields, empty arrangement, anchors
    absent for a validated version, anchors not `module+0xoffset@buildid`
    triples, non-list `qt_validated`) — a
    malformed register never silently weakens the gate.

  **Report-type coverage.** Every type the detector can emit has a pinned
  expected verdict, demonstrated by a permanent fixture: data race (the
  register machinery), lock-order-inversion (parses; its mutex sections
  are attribution-scanned under the same rules — our frame → OURS;
  all-foreign → registrable shape; the known libdbus cycles are
  detector-suppressed, so an unsuppressed one lands on the knob), thread
  leak (type-scoped exception to creation-never-attributes: a leak's ONLY
  stack is its creation stack, which is its participation evidence — so
  for `thread leak` blocks the creation stack attributes; rationale: the
  question-1 ruling exempts creation stacks because they record
  provenance, but for a leak the creation IS the act), fatal-signal
  reports (anomaly, never registrable), and unknown/future types
  (fail-closed anomaly). A frame-less `Location is global`/`stack`
  descriptor line is inert metadata, never a phantom alloc record.

  The canary battery runs BEFORE the real selection in every job, each
  canary asserting its EXPECTED verdict through the real classifier: the
  pure canary (two instrumented threads, one unsynchronized int) must
  FAIL ours; the mixed canary (instrumented writes racing an
  uninstrumented helper DSO's intercepted `memmove` — the exact shape the
  rejected `ignore_noninstrumented_modules=1` flag swallowed 0/20 in the
  tsan-blanket-spike) must FAIL ours; the foreign canary (bare
  `connectToBus` worker-spawn — the registered bus-bind class) must
  classify foreign-registered (silent on a toolchain where Qt doesn't
  fire it: warn-and-continue — its fire rate is Qt's, not ours). A canary
  that produces zero reports fails the job itself (a dead detector proves
  nothing). The foreign canary links no libdbusqml and calls
  `QDBusConnection::connectToBus` directly, so every bus-bind-family fire
  presents all-foreign attribution stacks by construction of the unwind
  (question-1 ruling, tsan-register-tightening). The independent
  cross-check (`scripts/tsan-crosscheck.sh` — no shared code) re-greps
  the classifier's foreign blocks for our
  basenames in their attribution sections and must agree.

  The foreign register currently holds three classes, all the Qt bus-bind
  family: `qt6-dbus-bus-bind-worker-startup` (the `0x2ae895` report:
  main-thread `free()` inside `qDBusBindToApplication` — the
  once-per-process bus bind spawning the QDBusConnection worker — racing
  the just-spawned worker's `memmove`; both access stacks
  runtime-symbol-less, hence structurally beyond TSan's suppression
  matcher, hence the verdict layer) and the two captured `0x2d1ab3`
  allocator-site variants (`qt6-dbus-bus-bind-realloc-{worker,main}` —
  `QArrayData::reallocateUnaligned` racing the worker's `memmove`,
  differing only in which thread reallocs; they reach the verdict layer
  only in non-test binaries, where the detector layer's test-scoped
  suppression anchors don't apply). Classification on all three:
  *accepted-unresolved-Qt-internal-risk — NOT proven false positive*;
  falsifier: the B run (one-shot TSan-instrumented Qt build —
  `~/Documents/dbusqml/todos/TODO.md`), which now decides a three-entry
  family wholesale.
  Entries carry the suppression file's six-field evidence block plus
  `qt_validated` — the list of Qt versions the entry was proven on. A log
  whose Qt version (parsed from the QtTest config line) is absent from
  the list renders the entry INERT: it does not match — the block is
  unregistered-foreign, the gate FAILS, and the census names it
  (`entry <id> inert: needs revalidation on Qt <ver>`). This is
  invalidate-pending-revalidation, never a silent match. Revalidation
  procedure: run `scripts/tsan-gate` on the new Qt, confirm the census
  shows the same arrangement, capture the new build's site anchors AND
  their BuildIds from the revalidation run's blocks (mechanical — both
  are printed on every frame line), and append the version AND its
  anchor triples to the entry together — a one-small-PR,
  evidence-carrying change. Deliberate
  consequence: the first CI run on an unvalidated Qt (the 6.8.2 cell)
  FAILS red by design until its cells are validated — the
  red→validate→green loop is the honest sequence for a genuinely
  unvalidated surface (recorded in the register header so the first-push
  red is expected, not alarming). (Distinct from suppression entry 2.1:
  that entry anchors the bus-bind ALLOCATOR race — `new` vs `delete` —
  via its symbolized dispatch frame; this register entry is the
  task-teardown class the symbol matcher cannot see.)

  The sweep procedure (the scheduled unblanketed census): run
  `scripts/tsan-gate` on the fork and diff the census against the
  register — the same command CI runs, so the census is reproducible
  locally. The red-team falsifier (fable/gemini): attempts to construct a
  real dbusqml-caused race whose attribution stacks present all-foreign —
  the boundary held in every attempt shape this cycle (our frames always
  appear as callers in the attribution stacks); the residual (a
  dbusqml-caused race presenting all-foreign in principle) stays
  documented and tripwired by the census, per the register entry's
  honesty qualifier.

ASan gate = full ctest with
tests/lsan-suppressions.txt (Qt-internal exit-time noise only; zero ODR
— test binaries link libdbusqml.so exactly once).

## Threading contract (attach/detach)

Attach/detach for a given (connection, service) are consumer-serialized —
in practice both run on the QML/main thread, which is the only
configuration the teardown markers protect. A C++ consumer driving
attach/detach for the SAME name from two threads concurrently is
unsupported: the registry mutex guards the manager thread, not concurrent
consumer threads. Ownership notifications arrive on the main thread (the
T1 relay); adaptors on foreign threads get main-thread delivery + a loud
warning (unsupported, T2 contract).

Under a captured prefix (`captureSubtree: true`), messages under that
path are delivered to handlers in bus arrival order on the object's
thread. Nested event loops inside those handlers are unsupported: they
can reorder successors. Same contract as today's single-node dispatch.

# Wire oracle (Phase 3, §6.3) — raw-libdbus peer, Nemo dbustestd pattern

A Qt-free D-Bus peer (libdbus C API only): exposes `Echo` (returns its
string arg), `Repr` (returns the call's signature string), `Ping`
(returns "pong"), and — the point — a **per-serial reply counter**:
every reply the oracle emits is tallied by the incoming message serial,
queryable via `ReplyCount(serial)`.

The exactly-one-reply-per-serial invariant (§6.3, PARITY.md axiom) is
asserted THROUGH this oracle: it is not Qt marshalling both ways, so it
kills vacuous-pass classes structurally (a double-reply or a swallow
shows up as count != 1 for that serial).

Build: `cmake -S tests/wire-oracle -B build-oracle` (needs libdbus-1
dev headers; NOT part of the default build — the main suite must stay
Qt-only). Run under `dbus-run-session`; point the reply-count
assertion at the serial of the call under test.

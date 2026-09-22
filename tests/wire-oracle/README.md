# Wire oracle (Phase 3, §6.3) — raw-libdbus peer, Nemo dbustestd pattern

A Qt-free D-Bus peer (libdbus C API only): exposes `Echo` (returns its
string arg), `Repr` (returns the call's signature string), `Ping`
(returns "pong"), and — the point — two per-serial reply counters:

- `ReplyCount(serial)` — replies EMITTED by this oracle for an incoming
  call serial (the oracle's own service-handler direction; tallied in
  the Echo/Repr/Ping handlers). The pre-CF-7 contract, unchanged.
- `ReceivedCount(serial)` — replies RECEIVED by this oracle for one of
  its own outgoing call serials (the caller direction; tallied by the
  filter on every arriving METHOD_RETURN/METHOD_ERROR, keyed by the
  spec's `reply_serial` correlation header). This is the direction that
  observes an adaptor under test.

Caller mode: `CallAdaptor(service, path, iface, member) -> (serial,
got)` invokes the named (no-arg) adaptor method synchronously and
reports the outgoing serial; the harness then polls
`ReceivedCount(serial)` — 1 = exactly-one-reply, 0 = swallow, 2 =
double-reply. Sensitivity self-test: run `CallAdaptor` against a
deliberately double-replying QtDBus-side service (two METHOD_RETURNs
for one serial) and assert `ReceivedCount == 2` — if the oracle cannot
see a deliberate double-send, its `== 1` evidence is vacuous. Verified
2026-09-09: double-sender → 2, well-behaved Ping → 1.

`CallAdaptorNoReply(service, path, iface, member) -> serial` sends the
same no-arg call with `NO_REPLY_EXPECTED` and no pending call. Any
reply that arrives anyway is unsolicited; the filter tallies it by
`reply_serial`. `ReceivedCount(serial)` is then whether the callee
replied to a call that asked it not to (0 = B4-correct).

Build: `cmake -S tests/wire-oracle -B build-oracle` (needs libdbus-1
dev headers; NOT part of the default build — the main suite must stay
Qt-only). Run under `dbus-run-session`; point the reply-count
assertion at the serial of the call under test.

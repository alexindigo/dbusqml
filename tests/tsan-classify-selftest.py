#!/usr/bin/env python3
# tsan-classify-selftest.py — the classifier's hermetic unit suite
# (F-amended gate). Runs tsan-classify.py over the captured/mutation
# fixtures in tests/tsan-fixtures/ and asserts each verdict. No bus, no
# Qt — pure classifier + crosscheck correctness. Exit 0 only if every
# case matches its expected verdict.
#
# Cases (plan §3 "Fixture self-tests"):
#   real pure-ours log              -> FAIL (ours)
#   real bus-bind block WITH its ours-frames creation stack (verbatim tip
#     fire)                         -> PASS, foreign-registered
#   allocation-stack-only-ours      -> FAIL
#   mutation per attribution stack (access1, access2, alloc) -> FAIL each
#   mutation creation-stack-only    -> PASS, foreign-registered (the
#     boundary in the other direction — owner ruling, question-1)
#   unregistered-foreign (test-scoped empty register) -> FAIL
#   truncated log (WARNING, no SUMMARY)            -> FAIL (anomaly)
#   garbled log (WARNING text, zero parseable blocks) -> FAIL (anomaly)
#   crosscheck disagreement: a foreign-dumped block carrying an artifact
#     basename in an ATTRIBUTION section -> tsan-crosscheck.sh flags it
#   crosscheck control: the real foreign dump (ours-frames only in the
#     creation section) -> tsan-crosscheck.sh stays clean
#   astra demonstration fixtures (tsan-register-tightening, permanent):
#   arrangement collision — entry 1's pooled ingredients in a DIFFERENT
#     arrangement (interceptors swapped between the access stacks) -> FAIL
#     (unregistered-foreign; pooled-set matching would have matched)
#   stale validation — entry 1's genuine block in a log whose Qt version
#     is absent from the entry's qt_validated list -> FAIL, census names
#     the inert entry (invalidate-pending-revalidation)
#   tsan-gate-edgecases (v3) additions:
#   same arrangement with different SITE offsets -> FAIL + near-miss note
#     (astra's shape-is-not-location demonstration)
#   the two captured bus-bind family variants -> registered, PASS
#   report-type coverage: lock-order-inversion (parses+scans), thread leak
#     (creation stack IS attribution — the type-scoped exception), fatal
#     SEGV report (anomaly, never foreign/PASS), frame-less Location line
#     (inert metadata), unknown future type (fail-closed anomaly)
#   seams: spoofed mid-log WARNING, single-frame stack, unnamed worker,
#     multi-block mixed verdicts; register schema validation x5 (fail-closed
#     at load, error named)
#   tsan-buildid-fold (v3.1): anchor triples carry the module BuildId —
#     same-site-different-build -> FAIL (build identity differs); missing
#     BuildId in an attribution frame -> FAIL (build identity absent)
#
# Fixture fidelity (plan rule): fixtures derive from REAL captured logs;
# synthesized content only where the real shape is unreachable, and then
# marked with an in-file "# fixture-provenance:" line (parser-inert).
#
# Env: FIXTURES_DIR overrides the fixtures directory (used by the
# deliberately-broken-fixture transcript run). SELFTEST_BREAK=1 inverts
# one expected verdict... (not used; the break-one proof is a corrupted
# FIXTURES_DIR copy, so the runner itself carries no self-sabotage path).

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))  # tests/
REPO = os.path.dirname(HERE)
CLASSIFIER = os.path.join(HERE, "tsan-classify.py")
CROSSCHECK = os.path.join(REPO, "scripts", "tsan-crosscheck.sh")
REGISTER = os.path.join(REPO, "tests", "tsan-foreign-register.toml")
FIXTURES = os.environ.get("FIXTURES_DIR",
                          os.path.join(REPO, "tests", "tsan-fixtures"))

failures = []


def run_classifier(log, register=REGISTER):
    """-> (exitcode, stdout+stderr)"""
    dump = os.path.join(FIXTURES, ".selftest-foreign.dump")
    if os.path.exists(dump):
        os.unlink(dump)
    p = subprocess.run(
        ["python3", CLASSIFIER, "--register", register, "--dump-foreign", dump, log],
        capture_output=True, text=True)
    return p.returncode, p.stdout + p.stderr


def expect(name, log, want_fail, register=REGISTER, want_substr=None):
    rc, out = run_classifier(os.path.join(FIXTURES, log), register)
    ok = (rc != 0) == want_fail
    if ok and want_substr is not None:
        ok = want_substr in out
    if ok:
        print(f"PASS: {name}")
    else:
        failures.append(name)
        print(f"FAIL: {name} (rc={rc}, wanted {'FAIL' if want_fail else 'PASS'}"
              f"{'' if want_substr is None else f', missing [{want_substr}]'})")
        for line in out.splitlines()[-12:]:
            print(f"      {line}")


def expect_crosscheck(name, dump_text, want_flagged):
    path = os.path.join(FIXTURES, ".selftest-crosscheck.dump")
    with open(path, "w") as f:
        f.write(dump_text)
    p = subprocess.run(["bash", CROSSCHECK, path], capture_output=True, text=True)
    ok = (p.returncode != 0) == want_flagged
    if ok:
        print(f"PASS: {name}")
    else:
        failures.append(name)
        print(f"FAIL: {name} (crosscheck rc={p.returncode}, wanted "
              f"{'flagged' if want_flagged else 'clean'})")
        print(f"      {p.stdout.strip()} {p.stderr.strip()}")


def main():
    # 4. captured real logs
    expect("real pure-ours canary log fails", "ours-pure-canary.log", True)
    expect("real bus-bind suite block (ours creation stack) is foreign-registered",
           "foreign-bus-bind-suite.log", False, want_substr="VERDICT: PASS")
    expect("real bus-bind probe block is foreign-registered",
           "foreign-bus-bind-probe.log", False, want_substr="VERDICT: PASS")
    expect("real mixed canary log fails (ours)", "ours-mixed-canary.log", True)
    expect("allocation-stack-only ours fails", "ours-allocation-stack.log", True)

    # 5. mutation fixtures — both directions of the attribution boundary
    expect("mutation: our frame in access stack 1 -> FAIL", "mut-access1.log", True)
    expect("mutation: our frame in access stack 2 -> FAIL", "mut-access2.log", True)
    expect("mutation: our frame in allocation stack -> FAIL", "mut-alloc.log", True)
    expect("mutation: our frame ONLY in creation stack -> stays foreign",
           "mut-creation.log", False, want_substr="VERDICT: PASS")

    # 6. unregistered foreign (test-scoped register with zero entries)
    expect("unregistered foreign block -> FAIL (knob)", "foreign-bus-bind-suite.log",
           True, register=os.path.join(FIXTURES, "register-none.toml"),
           want_substr="UNREGISTERED")

    # 7. parser anomalies
    expect("truncated log (no SUMMARY) -> FAIL", "truncated.log", True)
    expect("garbled log (WARNING text, zero blocks) -> FAIL", "garbled.log", True)
    expect("zero blocks + nonzero detector exit (.rc) -> FAIL", "garbled-rc.log", True)

    # 7b. astra's demonstration fixtures (permanent) — fingerprint v2
    # arrangement-tightness + the qt_validated invalidate-pending gate
    expect("arrangement collision: pooled ingredients, swapped arrangement "
           "-> FAIL (unregistered)", "foreign-collision-arrangement.log",
           True, want_substr="UNREGISTERED")
    expect("stale qt validation: entry inert -> FAIL with census line",
           "foreign-bus-bind-stale-qt.log", True,
           want_substr="entry qt6-dbus-bus-bind-worker-startup inert: "
           "needs revalidation on Qt 6.8.2")

    # 7c. fingerprint v3 site anchors (E1) + the two registered family
    # variants (E2) — tsan-gate-edgecases, astra's final objection
    expect("same arrangement, different SITE offsets -> FAIL + near-miss note",
           "foreign-same-shape-different-site.log", True,
           want_substr="site anchors differ")
    expect("captured variant (worker realloc) is registered",
           "foreign-bus-bind-variant-worker-realloc.log", False,
           want_substr="VERDICT: PASS")
    expect("captured variant (main realloc) is registered",
           "foreign-bus-bind-variant-main-realloc.log", False,
           want_substr="VERDICT: PASS")

    # 7d. report-type coverage (E3) — every type the detector can emit has
    # a pinned expected verdict
    expect("lock-order-inversion block: parses, scans, all-foreign -> knob FAIL",
           "foreign-lock-order.log", True, want_substr="lock-order-inversion")
    expect("thread leak: creation stack IS the attribution stack (type-scoped "
           "exception) — our frames -> OURS FAIL",
           "ours-thread-leak.log", True, want_substr="OURS")
    expect("fatal-signal report (==N==ERROR ... SEGV) -> anomaly FAIL, "
           "never foreign/PASS", "fatal-segv.log", True,
           want_substr="fatal/error report")
    expect("frame-less Location-is-global line is inert metadata -> the real "
           "block still registers", "foreign-global-location.log", False,
           want_substr="VERDICT: PASS")
    expect("unknown/future report type -> anomaly FAIL (fail-closed)",
           "unknown-type.log", True, want_substr="unknown report type")

    # 7e. parser/register seams (E4)
    expect("spoofed WARNING line mid-log (no block follows) -> anomaly FAIL",
           "spoofed-warning.log", True, want_substr="truncated block")
    expect("single-frame interceptor-only stack -> below_module 'none' -> "
           "UNREGISTERED FAIL", "foreign-single-frame-stack.log", True,
           want_substr="UNREGISTERED")
    expect("unnamed worker -> thread 'worker' mismatches the named entry -> "
           "UNREGISTERED FAIL", "foreign-unnamed-worker.log", True,
           want_substr="UNREGISTERED")
    expect("multi-block log with mixed verdicts -> FAIL, all blocks censused",
           "mixed-verdicts.log", True, want_substr="OURS")

    # 7f. register schema validation (E4.1) — fail-closed at load, the
    # error named; a malformed register never silently weakens the gate
    for bad, needle in [
            ("register-bad-unknown-key.toml", "unknown keys"),
            ("register-bad-missing-field.toml", "missing required fields"),
            ("register-bad-empty-arrangement.toml", "non-empty list"),
            ("register-bad-anchors-missing.toml", "anchors absent for validated Qt 6.8.2"),
            ("register-bad-qt-scalar.toml", "qt_validated must be a list")]:
        expect(f"register schema: {bad} -> load FAIL naming the error",
               "foreign-bus-bind-suite.log", True,
               register=os.path.join(FIXTURES, bad), want_substr=needle)

    # 7g. build identity (v3.1, tsan-buildid-fold) — astra's fourth
    # objection: a version string is not a binary identity; the anchor
    # triple pins the content hash, missing identity fails closed
    expect("same site, different BUILD (BuildId edited) -> FAIL naming the "
           "build mismatch", "foreign-same-site-different-build.log", True,
           want_substr="build identity differs")
    expect("missing BuildId in an attribution frame -> fail-closed FAIL",
           "foreign-missing-buildid.log", True,
           want_substr="build identity absent")

    # 8. crosscheck: disagreement simulation + clean control
    rc, _ = run_classifier(os.path.join(FIXTURES, "foreign-bus-bind-suite.log"))
    foreign_dump = os.path.join(FIXTURES, ".selftest-foreign.dump")
    with open(foreign_dump) as f:
        real_foreign = f.read()
    expect_crosscheck("crosscheck: real foreign dump (creation-stack ours "
                      "frames) stays clean", real_foreign, False)
    bad = real_foreign.replace(
        "    #1 <null> <null> (libQt6Core.so.6+0x2ae895)",
        "    #1 DBusProxy::touchSharedState() /home/tester/dbusqml-tip/dbus.cpp:93 "
        "(libdbusqml.so+0x3eea99)",
        1)
    if bad == real_foreign:
        failures.append("crosscheck-fixture-construction")
        print("FAIL: crosscheck-fixture-construction (injection anchor not found)")
    else:
        expect_crosscheck("crosscheck: artifact basename in an attribution section "
                          "is flagged", bad, True)

    for junk in (".selftest-foreign.dump", ".selftest-crosscheck.dump"):
        p = os.path.join(FIXTURES, junk)
        if os.path.exists(p):
            os.unlink(p)

    print(f"\nselftest: {len(failures)} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

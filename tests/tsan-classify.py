#!/usr/bin/env python3
# tsan-classify.py — verdict-layer TSan report classifier (F-amended gate,
# road-to-one follow-up; plan: ~/Documents/dbusqml/plans/tsan-classifier-gate/;
# arrangement-tight fingerprints: tsan-register-tightening/).
#
# The detector records everything (the amended-B suppression file stays
# exactly as landed); THIS tool reads captured per-test logs, splits them
# into complete TSan report blocks, and renders the verdict:
#
#   - any block with one of OUR artifacts in an ATTRIBUTION stack (either
#     access stack, or the allocation/Location stack) -> FAIL.
#   - thread-CREATION stacks never attribute (owner ruling, question-1:
#     participation attributes, provenance doesn't) — their module
#     basenames are recorded per census line for drift visibility.
#   - a block whose every ATTRIBUTION-stack frame is foreign AND whose
#     ARRANGEMENT fingerprint matches a register entry validated on the
#     log's Qt version -> counted, censused, non-failing.
#   - a foreign block with NO register match -> FAIL (the knob is FAIL).
#     An arrangement match whose qt_validated list does not contain the
#     log's Qt version is INERT, not a match: unregistered-foreign -> FAIL,
#     census names the inert entry (invalidate-pending-revalidation).
#   - any ambiguity (unparseable/truncated block, <null> frame with no
#     recognizable module in an attribution stack, zero blocks when the raw
#     log contains the WARNING string, nonzero recorded detector exit with
#     zero blocks) -> FAIL.
#
# Fingerprint v2 (arrangement, not pools — astra's closing objection): an
# ORDERED per-attribution-stack arrangement. Each stack contributes one
# record: (role, interceptor, below_module, thread, access) — role is
# access-first/access-second/alloc in appearance order; interceptor is the
# frame-#0 TSan interceptor name ("none" if frame 0 is not an interceptor);
# below_module is the module basename of the first resolvable frame below
# the interceptor; thread is "main" or the acting worker's name parsed from
# the block's own creation headers ("worker" if unnamed); access is the
# "kind:size" from the stack's own header ("write:8", "read:4", ...). NO
# offsets, NO build IDs — the arrangement discriminates structure, not
# instruction addresses, so it ports across builds. Matching is exact on
# the ordered record list.
#
# Fingerprint v3 (tsan-gate-edgecases; astra's final objection — shape is
# not location): the arrangement gains per-validated-Qt-version SITE
# ANCHORS — (module basename, module-relative offset) of the racing frame
# below each stack's interceptor, e.g. libQt6Core.so.6+0x2ae895. Anchors
# are ASLR-independent and stable per Qt build (verified identical across
# every captured fire of a class); they are checked ONLY against the exact
# Qt version they were captured on — the invalidate-pending-revalidation
# gate already pays the cross-version maintenance, so the old portability
# objection to offsets no longer applies. Match requires ALL THREE layers:
# qt version validated (else inert, unchanged) AND arrangement equal AND
# the anchors for that version equal. (A log with no parseable Qt version
# never gates the version layer — the QtTest config line is the mechanism
# — and anchors for such a log match against ANY validated version's list:
# the block demonstrably came from one of the validated builds.)
#
# Fingerprint v3.1 (tsan-buildid-fold; astra's fourth — and structurally
# final — objection): a Qt VERSION string is not a binary identity — the
# same "6.11.2" can be rebuilt into a different binary where the
# registered offsets point at different code. Site anchors are therefore
# TRIPLES: (module basename, module-relative offset, module BuildId) —
# the BuildId is the content-hash identity already printed on every frame
# line ("(BuildId: 3e89a2b2…)"). Matching requires all anchor triples
# equal; a frame with no BuildId token in an attribution stack cannot
# prove binary identity -> the block is unregistered -> FAIL (fail-closed).
# Versionless logs (probes/canaries) now match by TRIPLE against any
# validated version's list — a versionless block can only match if its
# binary IS a validated build, proven by content hash, not assumed. The
# identity ladder is complete: type -> arrangement -> site -> binary
# identity; beyond it only run-variable values exist (addresses, pids,
# timestamps), which are structurally unmatchable.
#
# Report-type coverage: known types are "data race", "lock-order-inversion"
# and "thread leak"; any other WARNING type is an anomaly -> FAIL. For
# "thread leak" blocks the CREATION stack is the participation evidence (a
# leak's only stack is its creation stack) — type-scoped exception to the
# creation-never-attributes rule, documented in PARITY §9. A
# "==N==ERROR: ThreadSanitizer:" fatal-signal report or an orphan SUMMARY
# line is an anomaly -> FAIL (never foreign, never PASS).
#
# Exit code 0 only if zero OURS blocks and zero unregistered-foreign blocks
# and zero anomalies. The census (per-fingerprint counts, registered or
# not, creation context) prints always.
#
# Usage: tsan-classify.py [--register PATH] [--dump-foreign FILE] LOG...
#   --register     foreign-class register TOML (default: the repo file).
#   --dump-foreign write every foreign-classified block VERBATIM to FILE
#                  (the cross-check's input — tsan-crosscheck.sh).
#   Each LOG may have a sibling "<log>.rc" file with the producing
#   process's exit code; a nonzero rc with zero parsed blocks is an anomaly.

import re
import sys
import os

# Our instrumented artifacts (basename or path-segment mention in a frame
# line of an attribution stack => the block is OURS). ONE list, here.
ARTIFACT_PATTERNS = ["libdbusqml", "dbusqml", "test_", "fuzz_", "tsan_canary"]

# Recognized foreign module basenames (plan §3): Qt6 libs, libdbus-1, libc,
# libstdc++, libtsan, ld-linux, [vdso]. TSan interceptor frames are
# libtsan-module frames. Anything else in an attribution stack is
# unregistered-ambiguous -> fail-closed OURS.
FOREIGN_MODULE_RE = re.compile(
    r"^(libQt6\w*\.so(\.\d+)*|libdbus-1\.so(\.\d+)*|libc\.so(\.\d+)*|"
    r"libstdc\+\+\.so(\.\d+)*|libtsan\.so(\.\d+)*|ld-linux[\w\-]*\.so(\.\d+)*|"
    r"ld\.so(\.\d+)*|linux-vdso\.so(\.\d+)*|\[vdso\])$")

WARNING_RE = re.compile(r"^WARNING: ThreadSanitizer:\s*(.+?)\s*(\(pid=\d+\))?\s*$")
SUMMARY_RE = re.compile(r"^SUMMARY: ThreadSanitizer:")
FRAME_RE = re.compile(r"^\s+#(\d+)\s+(.*?)\s*$")
CREATION_HDR_RE = re.compile(r"^\s+Thread\s+\S+.*\bcreated by\b")
LOCATION_HDR_RE = re.compile(r"^\s+Location")
ACCESS_HDR_RE = re.compile(
    r"^\s+(Previous\s+)?((?:[Aa]tomic\s+)?[Ww]rite|(?:[Aa]tomic\s+)?[Rr]ead"
    r"|Mutex\s+M\d+)")
QTVERSION_RE = re.compile(r"Qt(?:Test library)?\s*(\d+\.\d+\.\d+)")
# Fingerprint-v2 arrangement parsing (from the block's own header lines).
ACCESS_INFO_RE = re.compile(
    r"^\s+(?:Previous\s+)?((?:[Aa]tomic\s+)?[Ww]rite|(?:[Aa]tomic\s+)?[Rr]ead)"
    r"(?:\s+of\s+size\s+(\d+))?")
LOCATION_INFO_RE = re.compile(r"(heap block|global|stack)(?:\s+of\s+size\s+(\d+))?")
THREAD_REF_RE = re.compile(r"\b(?:by|in)\s+(main thread|thread\s+(T\d+))")
THREAD_NAME_RE = re.compile(r"Thread\s+(T\d+)(?:\s+'([^']+)')?")
ERROR_RE = re.compile(r"^==\d+==ERROR: ThreadSanitizer:")
# Report types the gate has characterized handling for. Anything else is
# an anomaly -> FAIL (fail-closed against future TSan report types).
KNOWN_TYPE_RE = re.compile(r"^(data race|lock-order-inversion|thread leak)\b")
BUILDID_RE = re.compile(r"\(BuildId:\s*([0-9a-fA-F]+)\)")


def parse_buildid(rest):
    """The BuildId hex of a frame line, or None (absent = the frame cannot
    prove its binary identity)."""
    m = BUILDID_RE.search(rest)
    return m.group(1) if m else None

# Section kinds
ACCESS = "access"      # attribution
ALLOC = "alloc"        # attribution (Location/allocated-by stacks)
CREATION = "creation"  # never attributes; census context only
NONSTACK = "nonstack"  # headers like "Cycle in lock order graph", Hint, ...


def section_kind(header):
    if CREATION_HDR_RE.match(header):
        return CREATION
    if LOCATION_HDR_RE.match(header):
        return ALLOC
    if ACCESS_HDR_RE.match(header):
        return ACCESS
    return None  # decide later: frames present => unknown-attribution


def parse_module_offset(rest):
    """(module, offset) of a frame: basename and +0x<hex> of the rightmost
    '(<path>+0x<hex>)' group. offset is the '0x...' string or None."""
    mod = None
    off = None
    for m in re.finditer(r"\(([^()]+)\)", rest):
        inner = m.group(1)
        mm = re.match(r"^(.+?)\+(0x[0-9a-fA-F]+)$", inner)
        if mm:
            mod = os.path.basename(mm.group(1))
            off = mm.group(2)
    return mod, off


def parse_module(rest):
    """Module of a frame: basename of the rightmost '(<path>+0x<hex>)' group."""
    return parse_module_offset(rest)[0]


def parse_function(rest):
    """Best-effort function token of a frame (args stripped), '' if none."""
    # drop the module/offset group and trailing BuildId
    core = re.sub(r"\([^()]*\)", "", rest)
    core = core.replace("<null>", "").strip()
    if not core:
        return ""
    # strip source path tail ("func() /path/file.cpp:12" -> "func()")
    core = core.split("/")[0].strip() if "/" in core and "::" not in core else core
    # strip args
    core = core.split("(")[0].strip()
    return core


class Block:
    __slots__ = ("report_type", "sections", "raw")

    def __init__(self, report_type):
        self.report_type = report_type
        self.sections = []  # list of (kind, [frame lines])
        self.raw = []


def split_blocks(text):
    """Split log text into (blocks, anomalies). Block = WARNING..SUMMARY."""
    lines = text.splitlines()
    blocks = []
    anomalies = []
    cur = None
    for line in lines:
        if ERROR_RE.match(line):
            # A fatal-signal/hard-error report ("==N==ERROR:
            # ThreadSanitizer: SEGV ...") is never a registrable foreign
            # class — fail closed. (Its orphan SUMMARY is caught below too;
            # one anomaly per line is enough.)
            anomalies.append(f"fatal/error report line: {line.strip()[:80]}")
            continue
        wm = WARNING_RE.match(line)
        if wm:
            if cur is not None:
                anomalies.append("truncated block: new WARNING before SUMMARY")
                blocks.append(cur)
            cur = Block(wm.group(1))
            cur.raw.append(line)
            continue
        if cur is None:
            if SUMMARY_RE.match(line):
                anomalies.append("orphan SUMMARY line without an open block")
            continue
        cur.raw.append(line)
        if SUMMARY_RE.match(line):
            blocks.append(cur)
            cur = None
    if cur is not None:
        anomalies.append("truncated block: WARNING without SUMMARY")
        blocks.append(cur)
    if "WARNING: ThreadSanitizer" in text and not blocks:
        anomalies.append("log contains WARNING but zero parsed blocks")
    return blocks, anomalies


def sectionize(block):
    """Fill block.sections from block.raw; returns anomaly or None."""
    sections = []
    cur_header = None
    cur_frames = []

    def flush():
        nonlocal cur_header, cur_frames
        if cur_header is None and not cur_frames:
            return
        kind = section_kind(cur_header or "")
        if kind is None:
            kind = ACCESS if cur_frames else NONSTACK
            # unknown frame-bearing section: fail-closed attribution
        sections.append((kind, cur_header or "", list(cur_frames)))
        cur_header = None
        cur_frames = []

    for line in block.raw[1:]:  # skip the WARNING line itself
        if SUMMARY_RE.match(line):
            break
        fm = FRAME_RE.match(line)
        if fm:
            cur_frames.append(line)
            continue
        if not line.strip():
            flush()
            continue
        flush()
        cur_header = line
    flush()
    block.sections = sections
    return None


ORDINAL = {1: "first", 2: "second"}


def thread_context(header, thread_names):
    """'main' | the worker's creation-header name | 'worker' (unnamed)."""
    tm = THREAD_REF_RE.search(header)
    if not tm:
        return ""
    if tm.group(1) == "main thread":
        return "main"
    return thread_names.get(tm.group(2), "worker")


def build_arrangement(block):
    """Ordered per-attribution-stack arrangement (fingerprint v2) plus the
    per-stack site anchors (v3).

    Returns (records, anchors):
    - records: one tuple per attribution stack, in appearance order:
      (role, interceptor, below_module, thread, access).
    - anchors: one "module+0xoffset@buildid" triple string per attribution
      stack (same order) — the racing frame below the interceptor. A frame
      whose BuildId (or offset) token is absent cannot prove binary
      identity: the anchor is marked "@NONE" and never matches (fail-closed).
      A stack with no resolvable below-frame anchors as "none".
    Returns (None, None) if an attribution stack carries no frames (caller
    treats as ambiguous).

    Type-scoped exception (question-1 ruling's edge, plan E3.2): for
    "thread leak" blocks the CREATION stack IS the participation evidence
    (a leak's only stack is its creation stack), so it attributes here.
    """
    # T-id -> name map from the block's own creation headers
    # ("Thread T3 'QDBusConnection' (tid=..., finished) created by ...").
    thread_names = {}
    for kind, header, _frames in block.sections:
        if kind == CREATION:
            nm = THREAD_NAME_RE.search(header)
            if nm and nm.group(2):
                thread_names[nm.group(1)] = nm.group(2)

    leak_attribution = block.report_type == "thread leak"
    records = []
    anchors = []
    access_n = 0
    alloc_n = 0
    creation_n = 0
    for kind, header, frames in block.sections:
        if kind == CREATION and not leak_attribution:
            continue
        if not frames:
            # An ACCESS section without frames is a truncation smell ->
            # ambiguous (fail-closed). A LOCATION section without frames
            # ("Location is global 'x' of size N at 0x.. (mod+0x..)" — a
            # one-line descriptor, not a stack) is metadata: skip it; the
            # role list simply gains no alloc record (plan E3.4). A
            # thread-leak creation stack without frames: same truncation
            # smell as an empty access stack.
            if kind == ALLOC:
                continue
            return None, None
        if kind == CREATION:
            # thread leak: the creation stack attributes; the acting
            # thread is the CREATED thread named in the header itself.
            creation_n += 1
            role = "creation" if creation_n == 1 else f"creation-{creation_n}"
            nm = THREAD_NAME_RE.search(header)
            thread = (nm.group(2) if nm and nm.group(2) else "worker")
            access = ""
        elif kind == ACCESS:
            access_n += 1
            role = f"access-{ORDINAL.get(access_n, str(access_n))}"
            im = ACCESS_INFO_RE.match(header)
            if im:
                kind_word = " ".join(im.group(1).lower().split())
                access = f"{kind_word}:{im.group(2)}" if im.group(2) else kind_word
            else:
                access = ""  # e.g. "Mutex M..." headers carry no size
            thread = thread_context(header, thread_names)
        elif kind == ALLOC:
            alloc_n += 1
            role = "alloc" if alloc_n == 1 else f"alloc-{alloc_n}"
            lm = LOCATION_INFO_RE.search(header)
            if lm:
                kind_word = lm.group(1).split()[0]
                access = f"{kind_word}:{lm.group(2)}" if lm.group(2) else kind_word
            else:
                access = ""
            thread = thread_context(header, thread_names)
        else:
            continue
        if not frames:
            return None, None
        # frame #0: interceptor slot. A libtsan-module frame is an
        # interceptor frame; its function token is the interceptor name.
        f0 = FRAME_RE.match(frames[0]).group(2)
        f0_mod = parse_module(f0)
        if f0_mod and f0_mod.startswith("libtsan"):
            interceptor = parse_function(f0) or "unnamed"
            below = frames[1:]
        else:
            interceptor = "none"
            below = frames
        below_module = "none"
        anchor = "none"
        for fl in below:
            frest = FRAME_RE.match(fl).group(2)
            m, off = parse_module_offset(frest)
            if m:
                below_module = m
                bid = parse_buildid(frest)
                if off and bid:
                    anchor = f"{m}+{off}@{bid}"
                else:
                    # module resolved but offset/BuildId absent: the frame
                    # cannot prove its binary identity — fail-closed at the
                    # anchor layer ("@NONE" never matches a register entry).
                    anchor = f"{m}+{off or '?'}@NONE"
                break
        records.append((role, interceptor, below_module, thread, access))
        anchors.append(anchor)
    return tuple(records), tuple(anchors)


def classify_block(block):
    """-> (verdict, fingerprint, anchors, creation_modules, reason)
    verdict in {"ours", "foreign", "ambiguous"}.
    fingerprint = (report_type, arrangement); anchors = the ordered
    per-stack "module+0xoffset" site list — both computed over ATTRIBUTION
    sections only (creation stacks are census context — owner ruling,
    question-1; EXCEPT for "thread leak" blocks, where the creation stack
    is the participation evidence and therefore attributes — plan E3.2).
    """
    creation_modules = set()
    leak_attribution = block.report_type == "thread leak"

    for kind, _header, frames in block.sections:
        is_creation = kind == CREATION and not leak_attribution
        for line in frames:
            fm = FRAME_RE.match(line)
            rest = fm.group(2)
            mod = parse_module(rest)
            if is_creation:
                if mod:
                    creation_modules.add(mod)
                continue
            if kind == NONSTACK:
                continue
            # attribution sections (access, alloc, unknown-frame-bearing;
            # creation for thread-leak blocks)
            if any(p in line for p in ARTIFACT_PATTERNS):
                return "ours", None, None, sorted(creation_modules), \
                    "our artifact in an attribution stack"
            if mod is None:
                return "ambiguous", None, None, sorted(creation_modules), \
                    "module-less frame in an attribution stack"
            if not FOREIGN_MODULE_RE.match(mod):
                return "ambiguous", None, None, sorted(creation_modules), \
                    f"unrecognized module '{mod}' in an attribution stack"

    arrangement, anchors = build_arrangement(block)
    if not arrangement:
        return "ambiguous", None, None, sorted(creation_modules), \
            "no frames in any attribution stack"
    return "foreign", (block.report_type, arrangement), anchors, \
        sorted(creation_modules), ""


CLASS_ALLOWED_KEYS = {
    "id", "report_type", "classification", "qt_validated", "arrangement",
    "anchors", "summary_class", "addr2line", "reproducer", "owning_test",
    "parent_parity", "upstream_link", "honesty_qualifier", "falsifier",
}
CLASS_REQUIRED_KEYS = CLASS_ALLOWED_KEYS - {"anchors"}  # anchors checked below
ARRANGEMENT_KEYS = {"role", "interceptor", "below_module", "thread", "access"}


class RegisterSchemaError(ValueError):
    pass


def _schema_fail(entry_id, msg):
    raise RegisterSchemaError(f"register entry '{entry_id}': {msg}")


def anchor_mismatch_reason(block_anchors, entry_site_lists):
    """Why a same-arrangement anchor comparison failed (v3.1 triples):
    build identity absent (the block can't prove it) | build identity
    differs (same site, different binary) | site anchors differ (a
    different site entirely)."""
    if any(a.endswith("@NONE") or a == "none" for a in block_anchors):
        return "build identity absent"
    block_sites = [a.split("@")[0] for a in block_anchors]
    if any(block_sites == [s.split("@")[0] for s in sites]
           for sites in entry_site_lists):
        return "build identity differs"
    return "site anchors differ"


ANCHOR_TRIPLE_RE = re.compile(r"^[^+()]+\+0x[0-9a-fA-F]+@[0-9a-fA-F]{8,}$")


def load_register(path):
    """Load + validate the register, FAIL-CLOSED: any malformation is a
    RegisterSchemaError naming the entry and field (a malformed register
    must never silently weaken the gate — plan E4.1)."""
    import tomllib
    with open(path, "rb") as f:
        data = tomllib.load(f)
    if set(data.keys()) - {"class"}:
        raise RegisterSchemaError(
            f"unknown top-level keys: {sorted(set(data.keys()) - {'class'})}")
    entries = []
    for c in data.get("class", []):
        eid = c.get("id", "?")
        unknown = set(c.keys()) - CLASS_ALLOWED_KEYS
        if unknown:
            _schema_fail(eid, f"unknown keys: {sorted(unknown)}")
        missing = [k for k in CLASS_REQUIRED_KEYS if k not in c]
        if missing:
            _schema_fail(eid, f"missing required fields: {missing}")
        if not isinstance(c["qt_validated"], list) or \
                not all(isinstance(v, str) and v for v in c["qt_validated"]):
            _schema_fail(eid, "qt_validated must be a list of version strings")
        qt_validated = tuple(c["qt_validated"])
        arr = c["arrangement"]
        if not isinstance(arr, list) or not arr:
            _schema_fail(eid, "arrangement must be a non-empty list")
        records = []
        for i, a in enumerate(arr):
            if set(a.keys()) != ARRANGEMENT_KEYS:
                _schema_fail(eid, f"arrangement[{i}] keys must be exactly "
                             f"{sorted(ARRANGEMENT_KEYS)}, got {sorted(a.keys())}")
            if not all(isinstance(a[k], str) for k in ARRANGEMENT_KEYS) or \
                    not a["role"]:
                _schema_fail(eid, f"arrangement[{i}] fields must be strings, "
                             "role non-empty")
            records.append((a["role"], a["interceptor"], a["below_module"],
                            a["thread"], a["access"]))
        anchors = c.get("anchors", {})
        if not isinstance(anchors, dict):
            _schema_fail(eid, "anchors must be a table keyed by Qt version")
        site_map = {}
        for v in qt_validated:
            if v not in anchors:
                _schema_fail(eid, f"anchors absent for validated Qt {v}")
            sites = anchors[v]
            if not isinstance(sites, list) or len(sites) != len(records) or \
                    not all(isinstance(s, str) and s for s in sites):
                _schema_fail(eid, f"anchors[\"{v}\"] must be a list of "
                             f"{len(records)} site strings (one per "
                             "arrangement record)")
            bad = [s for s in sites if not ANCHOR_TRIPLE_RE.match(s)]
            if bad:
                _schema_fail(eid, f"anchors[\"{v}\"] entries must be "
                             f"'module+0xoffset@buildid' triples, got: {bad}")
            site_map[v] = tuple(sites)
        entries.append({
            "id": eid,
            "report_type": c["report_type"],
            "arrangement": tuple(records),
            "qt_validated": qt_validated,
            "anchors": site_map,
            "classification": c["classification"],
        })
    return entries


def main(argv):
    register_path = None
    dump_foreign = None
    logs = []
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--register":
            register_path = argv[i + 1]
            i += 2
        elif a == "--dump-foreign":
            dump_foreign = argv[i + 1]
            i += 2
        else:
            logs.append(a)
            i += 1
    if not logs:
        print("usage: tsan-classify.py [--register PATH] [--dump-foreign FILE] LOG...",
              file=sys.stderr)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    if register_path is None:
        register_path = os.path.join(here, "tsan-foreign-register.toml")
    try:
        register = load_register(register_path)
    except Exception as e:
        print(f"tsan-classify: cannot load register {register_path}: {e}", file=sys.stderr)
        return 2

    total_ours = 0
    total_unregistered = 0
    total_anomalies = 0
    census = {}  # fp -> {count, registered, id, creation_contexts, qt_mismatch}
    foreign_blocks = []

    for log in logs:
        try:
            with open(log, "r", errors="replace") as f:
                text = f.read()
        except OSError as e:
            print(f"tsan-classify: cannot read {log}: {e}", file=sys.stderr)
            total_anomalies += 1
            continue
        qt_version = None
        qm = QTVERSION_RE.search(text)
        if qm:
            qt_version = qm.group(1)
        blocks, anomalies = split_blocks(text)
        total_anomalies += len(anomalies)
        for a in anomalies:
            print(f"ANOMALY [{os.path.basename(log)}]: {a}")
        rc_path = log + ".rc"
        if os.path.exists(rc_path):
            try:
                rc = int(open(rc_path).read().strip())
                if rc != 0 and not blocks:
                    print(f"ANOMALY [{os.path.basename(log)}]: nonzero detector "
                          f"exit ({rc}) with zero parsed blocks")
                    total_anomalies += 1
            except ValueError:
                print(f"ANOMALY [{os.path.basename(log)}]: unreadable .rc")
                total_anomalies += 1
        for b in blocks:
            sectionize(b)
            if not KNOWN_TYPE_RE.match(b.report_type):
                total_anomalies += 1
                print(f"ANOMALY [{os.path.basename(log)}] unknown report type "
                      f"'{b.report_type}' — fail-closed")
                continue
            verdict, fp, anchors, creation_mods, reason = classify_block(b)
            if verdict == "ours":
                total_ours += 1
                print(f"OURS [{os.path.basename(log)}] {b.report_type}: {reason}")
                print("\n".join("    " + l for l in b.raw[:14]))
            elif verdict == "ambiguous":
                total_ours += 1  # fail-closed
                print(f"AMBIGUOUS->FAIL [{os.path.basename(log)}] {b.report_type}: {reason}")
                print("\n".join("    " + l for l in b.raw[:14]))
            else:
                # Match layers: arrangement equal AND qt version validated
                # (else inert) AND the anchor TRIPLES (module, offset,
                # BuildId) for that version equal. A versionless log
                # matches by triple against any validated version's list —
                # its binary must BE a validated build, proven by content
                # hash. An arrangement+version match with unequal triples
                # is a near-miss: unregistered (FAIL), the census names
                # which of the three ways it missed.
                entry = None
                inert = None
                near_miss = None  # (entry, reason)
                for e in register:
                    if (e["report_type"], e["arrangement"]) != fp:
                        continue
                    if e["qt_validated"] and qt_version and \
                            qt_version not in e["qt_validated"]:
                        inert = e
                        continue
                    if qt_version:
                        candidate_lists = [e["anchors"][qt_version]]
                    else:
                        candidate_lists = [e["anchors"][v]
                                           for v in e["qt_validated"]]
                    if not any(anchors == sites for sites in candidate_lists):
                        near_miss = (e, anchor_mismatch_reason(
                            anchors, candidate_lists))
                        continue
                    entry = e
                    break
                foreign_blocks.append((os.path.basename(log), b))
                shown = entry or inert or (near_miss and near_miss[0])
                key = (fp, anchors, shown["id"] if shown else None,
                       entry is not None)
                if key not in census:
                    census[key] = {"count": 0, "registered": entry is not None,
                                   "id": shown["id"] if shown else "-",
                                   "classification": shown["classification"] if shown else "-",
                                   "inert": inert is not None and entry is None,
                                   "near_miss": near_miss[0]["id"] if near_miss
                                   and entry is None else None,
                                   "near_miss_reason": near_miss[1] if near_miss
                                   and entry is None else None,
                                   "qt": qt_version,
                                   "creation": set()}
                census[key]["count"] += 1
                census[key]["creation"].update(creation_mods)
                if entry is None:
                    total_unregistered += 1

    print("\n=== TSan classifier census ===")
    if not census:
        print("(no foreign blocks)")
    for (fp, anchors, _, _), c in sorted(
            census.items(),
            key=lambda kv: (kv[0][2] or "", kv[0][0], kv[0][1] or ())):
        reg = "registered" if c["registered"] else "UNREGISTERED"
        print(f"{c['count']:4d}  {reg:12} id={c['id']}")
        print(f"      type={fp[0]}")
        print("      arrangement=[" + "; ".join(
            f"{r[0]} {r[1]}->{r[2]} ({r[3]}, {r[4]})" for r in fp[1]) + "]")
        print(f"      anchors={list(anchors)}")
        print(f"      creation_context={sorted(c['creation'])}")
        print(f"      classification={c['classification']}")
        if c["inert"]:
            print(f"      entry {c['id']} inert: needs revalidation on Qt "
                  f"{c['qt'] or '?'}")
        if c["near_miss"]:
            print(f"      note: arrangement matches entry '{c['near_miss']}' "
                  f"but {c['near_miss_reason']} — not the registered incident")
        if not c["registered"]:
            print("      -> FAIL (knob: unregistered foreign class fails — register it "
                  "with an evidence block in the same change)")
    print(f"blocks: ours={total_ours} foreign_registered="
          f"{sum(c['count'] for k, c in census.items() if c['registered'])} "
          f"foreign_unregistered={total_unregistered} anomalies={total_anomalies}")

    if dump_foreign is not None:
        with open(dump_foreign, "w") as f:
            for logname, b in foreign_blocks:
                f.write(f"### foreign block from {logname}\n")
                f.write("\n".join(b.raw))
                f.write("\n")

    if total_ours or total_unregistered or total_anomalies:
        print("VERDICT: FAIL")
        return 1
    print("VERDICT: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

#!/usr/bin/env python3
# generate-mutations.py — build the mutation/synthetic fixtures from the
# captured real logs in this directory. Run once per regeneration; the
# outputs are committed fixtures (tests/tsan-fixtures/). The injected frame
# mirrors the real 0x9df2f report's libdbusqml frame shape (council-punch
# p1-tsan-test_dbus-testConnectionLossHandling.log).
import pathlib

HERE = pathlib.Path(__file__).parent
BASE = (HERE / "foreign-bus-bind-suite.log").read_text().splitlines()

INJECT = ("    #2 DBusProxy::touchSharedState() "
          "/home/tester/dbusqml-tip/dbus.cpp:93 (libdbusqml.so+0x3eea99) "
          "(BuildId: 94c7367a48f7ce92871b497d8bc10a2d950cd45e)")


def find_section_bounds(lines, header_startswith):
    """Return (first_frame_idx, last_frame_idx) of the section whose header
    starts with the given text."""
    hdr = next(i for i, l in enumerate(lines) if l.strip().startswith(header_startswith))
    first = hdr + 1
    last = first
    while last < len(lines) and lines[last].strip().startswith("#"):
        last += 1
    return first, last


def inject_into(lines, header_startswith, out_name):
    first, last = find_section_bounds(lines, header_startswith)
    out = lines[:last] + [INJECT] + lines[last:]
    (HERE / out_name).write_text("\n".join(out) + "\n")
    print(f"wrote {out_name}")


# 1-2. Access-stack injections (the two access stacks of the bus-bind
# block) — each must flip the verdict to FAIL.
inject_into(BASE, "Write of size", "mut-access1.log")
inject_into(BASE, "Previous write of size", "mut-access2.log")

# 3. Allocation-stack-only injection (the 0x9df2f shape, synthesized): a
# Location section carrying our frame, access stacks untouched -> FAIL.
alloc_section = """
  Location is heap block of size 112 at 0x72100000c9d0 allocated by main thread:
    #0 realloc <null> (libtsan.so.2+0x9615f) (BuildId: fad1b3f53c846e11570baa72e153cca909e3f7d2)
    #1 DBusProxy::touchSharedState() /home/tester/dbusqml-tip/dbus.cpp:93 (libdbusqml.so+0x3eea99) (BuildId: 94c7367a48f7ce92871b497d8bc10a2d950cd45e)
    #2 <null> <null> (libc.so.6+0x27780) (BuildId: 503200d7fda94a5dc6058d7e0694e5d1dcb2e372)
"""
out = []
for line in BASE:
    if line.startswith("SUMMARY:"):
        out.append(alloc_section)
    out.append(line)
(HERE / "mut-alloc.log").write_text("\n".join(out))
print("wrote mut-alloc.log")
# The plan's standalone allocation-stack fixture shares the construction.
(HERE / "ours-allocation-stack.log").write_text("\n".join(out))
print("wrote ours-allocation-stack.log")

# 4. Creation-stack-only injection — the verdict must STAY
# foreign-registered (the boundary proof in the other direction).
first, last = find_section_bounds(BASE, "Thread T3")
out = BASE[:last] + [INJECT] + BASE[last:]
(HERE / "mut-creation.log").write_text("\n".join(out) + "\n")
print("wrote mut-creation.log")

# 5. Truncated: WARNING present, block never terminated by SUMMARY.
out = [l for l in BASE[:29]]  # through the creation stack, no SUMMARY
(HERE / "truncated.log").write_text("\n".join(out) + "\n")
print("wrote truncated.log")

# 6. Garbled: the WARNING string present mid-line, zero parseable blocks.
(HERE / "garbled.log").write_text(
    "totally garbled prefix WARNING: ThreadSanitizer data race but no real block\n"
    "junk line\nnot a frame\n")
print("wrote garbled.log")

# 7. Test-scoped register WITHOUT the bus-bind entry (for the
# unregistered-foreign knob proof).
(HERE / "register-none.toml").write_text(
    "# test-scoped register copy with NO entries — every foreign block is\n"
    "# unregistered here (the knob must FAIL).\n")
print("wrote register-none.toml")

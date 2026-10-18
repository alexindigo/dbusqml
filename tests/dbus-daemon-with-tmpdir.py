#!/usr/bin/env python3
"""Launch test buses with one TMPDIR-aware listen-address policy.

Use directly wherever a fixture starts dbus-daemon, or pass this executable to
dbus-run-session's --dbus-daemon option. exec preserves daemon PID, inherited
print-address descriptors, exit status and the fixtures' termination semantics.
The installed session configuration still supplies the ordinary bus policy;
--address overrides only its fixed socket location.
"""
import os
import sys
import tempfile
from urllib.parse import quote_from_bytes

address = "unix:tmpdir=" + quote_from_bytes(os.fsencode(tempfile.gettempdir()), safe="/")
os.execvp("dbus-daemon", ["dbus-daemon", "--address=" + address, *sys.argv[1:]])

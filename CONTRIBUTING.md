# Contributing to dbusqml

Thanks for taking an interest. This is a small standalone library — the
contribution loop is short.

## Build

Requires **CMake ≥ 3.16** and **Qt ≥ 6.8**. A standard Qt install
covers all needed components (`Core`, `Qml`, `DBus`, `Test`).

```sh
scripts/build            # release build to build-release/
scripts/build-dev        # debug build to build-debug/
scripts/install          # install into Qt's QML import path
```

Or by hand:

```sh
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
```

## Test

C++ tests use `QTest`; QML tests use `qmltestrunner`. Both need a
running D-Bus session — the scripts wrap them with `dbus-run-session`.

```sh
scripts/run-tests        # C++ + QML, dbus session provided
scripts/run-qml-tests    # QML only
```

Coverage:

```sh
cmake -B build-cov -DENABLE_COVERAGE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cov
scripts/run-tests build-cov
gcovr -r . --exclude 'tests/.*' --exclude 'build-.*' --html-details coverage/index.html
```

## Add a bundled type descriptor

If you'd like `DBus {}` to know about a new interface out of the box,
drop the D-Bus introspection XML into `types/`, add it to the
`qt_add_resources(dbusqml "catalog" ... FILES ...)` list in
`CMakeLists.txt`, and add a short entry to `docs/TYPES.md`. Interface
descriptor XML follows the standard D-Bus format — you can usually
copy-paste from the freedesktop spec.

## Commits & branches

- Small, focused commits. One user-visible change per commit; unrelated
  cleanups go into their own "chore:" commit.
- Commit messages: imperative subject ≤ 72 chars, blank line, prose
  body explaining *why* not *what*. Existing history has examples.
- Master is the release branch. Send PRs against `master`.

## Signing

Commits and tags in this repo are SSH-signed (see `.gitconfig`). Not
required for external PRs — the maintainer will sign the merge.

## Filing issues

Please include:
- Qt version (`qmake6 -v`)
- OS + version
- The QML snippet that reproduces the problem
- Full stderr — many diagnostics are `qWarning()` and only surface
  when `QT_FORCE_STDERR_LOGGING=1` is set

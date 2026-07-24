# dbusqml — Development

## Devcontainer

A devcontainer is provided at `.devcontainer/` for consistent tooling
across contributors. It uses Arch Linux `base-devel` and includes:

- Qt6 (`qt6-base`, `qt6-declarative`) — `qml6`, `qmllint`, `qmlformat`
- Build tools — `cmake`, `ninja`, `clang` (also provides `clang-format`)
- `quickshell` — used by the reactive-binding comparison spike
- `dbus` — `gdbus` and system-bus client libs
- Shell tooling — `shellcheck`, `shfmt`

**Editors:**

- **VSCode** — auto-detects `.devcontainer/` and prompts to "Reopen in Container"
- **CLI** — build and run the image directly:

  ```
  docker build .devcontainer/ -t dbusqml-dev:local
  docker run -it --rm \
    --network=host \
    -v /run/dbus:/run/dbus \
    -v "$(pwd):/workspaces/dbusqml" \
    -w /workspaces/dbusqml \
    dbusqml-dev:local bash
  ```

**What runs in the container:** linting, formatting, in-container builds,
DBus introspection (via host's system bus), Quickshell comparison spike.

**What stays on host:** git (commits, signing via 1Password), production
builds, integration testing against user's real DBus environment.

## Building

```
scripts/build           # release build (build-release/)
scripts/build-dev       # dev build with warnings enabled
scripts/build-test      # build with test targets
```

Builds happen on the host by default. To build inside the devcontainer:

```
docker run --rm -v "$(pwd):/workspaces/dbusqml" -w /workspaces/dbusqml \
  dbusqml-dev:local scripts/build
```

## Testing

```
scripts/run-tests       # C++ unit tests (test_dbustypes, test_dbusconnection)
scripts/run-qml-tests   # QML tests via qmltestrunner
```

## QML Linting and Formatting

Inside devcontainer:

```
qmllint tests/*.qml examples/**/*.qml
qmlformat -n tests/*.qml
```

Severity levels configured in `.qmllint.ini` at the repo root. See file
comments for what's relaxed and what's kept strict.

## C++ Formatting

Repo uses `.clang-format` (Qt style). To check formatting:

```
clang-format --dry-run --Werror *.cpp *.h
```

To auto-format:

```
clang-format -i *.cpp *.h
```

## Pre-commit Hook

A tracked pre-commit hook lives at `.githooks/pre-commit`. On each commit
it runs qmllint, qmlformat, clang-format, shellcheck, and shfmt across
the entire repo (not just staged files), blocking commits with errors.

**Docker required.** The hook builds and runs the devcontainer image to
execute checks. First run builds the image (~1-2 minutes); subsequent
runs reuse the cached image.

To activate the hook on your clone (one-time setup):

```
git config core.hooksPath .githooks
```

To bypass a specific commit (e.g., Docker not available):

```
git commit --no-verify
```

To restore default behavior:

```
git config --unset core.hooksPath
```

## Quickshell Reactive-Binding Spike

The `tests/qs-comparison/` directory contains a diagnostic harness that
runs latest Quickshell against `org.freedesktop.NetworkManager` inside
the devcontainer, to verify whether QS exhibits the same reactive-binding
behavior dbusqml sees with `QQmlPropertyMap`. See
`tests/qs-comparison/README.md`.

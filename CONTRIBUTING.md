# Contributing

## Building

Install a C compiler, Meson, Ninja, and the required development dependencies,
including GTK 4, Libadwaita, GStreamer, JSON-GLib, Wayland client,
and `wayland-protocols`, then configure and build the project:

```sh
meson setup build --buildtype=debug
meson compile -C build
meson devenv -C build kasasa
```

Use `./tools/run-debug` for a reproducible log, Wayland, GStreamer, or gdb
session. The complete troubleshooting runbook is in
[`docs/debugging.md`](docs/debugging.md).

## Testing

Configure and build the project, then run the full test suite:

```sh
meson setup build
meson compile -C build
meson test -C build --print-errorlogs
```

Run only the fast, display-independent unit tests while developing:

```sh
meson test -C build --suite unit --print-errorlogs
```

GTK component tests do not contact Portal or PipeWire, but they need a display.
Run them in the current desktop session, or under Xvfb in a headless environment:

```sh
meson test -C build --suite gtk --print-errorlogs
```

Before submitting lifecycle or native-capture changes, run an instrumented
build as well:

```sh
meson setup build-sanitize --buildtype=debug -Db_sanitize=address,undefined
meson compile -C build-sanitize
ASAN_OPTIONS=detect_leaks=0 meson test -C build-sanitize --print-errorlogs
valgrind --leak-check=full --errors-for-leak-kinds=definite \
  --track-fds=yes --error-exitcode=1 build/tests/test-hyprland-capture
```

The CI workflow also verifies a staged `meson install` so packaging regressions
are caught before release.

Tests that require a real Wayland display managed by Hyprland are opt-in and are
not run by regular CI:

```sh
meson setup build-integration -Dintegration_tests=true
meson compile -C build-integration
meson test -C build-integration --suite integration --print-errorlogs
```

This suite performs a real monitor first-frame check and creates a temporary
window for a DMA-BUF-or-wl_shm capture and stop check. It must run in the host
Hyprland session; a restricted sandbox may not be able to reach the compositor
socket. Set `KASASA_INTEGRATION_OUTPUT` when the first GDK monitor is not the
output under investigation. Layer-shell support is skipped when unavailable.

## Translating

1. Fork the repository and clone it
2. Take the `po/kasasa.pot` file and generate a new `.po` translation file from it; also add the new translation language to `po/LINGUAS`
3. Submit a PR with the changed files

_Note: you can use any gettext-compatible translation editor._

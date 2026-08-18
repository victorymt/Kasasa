# Debugging Kasasa

Use a debug build from the Meson development environment. The development
environment supplies the uninstalled GSettings schema and the build output
directories:

```sh
meson setup build --buildtype=debug
meson compile -C build
meson devenv -C build kasasa
```

The repository includes a small wrapper for repeatable diagnostic modes:

```sh
./tools/run-debug --logs -- --screencast
./tools/run-debug --wayland -- --list-windows --json
./tools/run-debug --gst -- --screencast
./tools/run-debug --shm -- --screencast
./tools/run-debug --gdb -- --screencast
meson devenv -C build kasasa --diagnostics
```

`--logs` enables GLib info/debug messages. `--wayland` prints the Wayland
client protocol, which is high volume and should be used for a short capture.
The capture worker logs Wayland setup stages, first-frame latency, DMA-BUF pool
drops, retry failures, and stop/join duration at debug level.
`--gst` enables GStreamer logging, pipeline state/warning/async/QOS diagnostics,
and writes pipeline dot files under `build/gst-debug`; render them with
Graphviz when available. It also records first-frame timing and appsrc flow
results.
`--shm` sets `KASASA_DISABLE_DMABUF=1` so window capture exercises the wl_shm
fallback even when the compositor and GPU support DMA-BUF.
Unset the variable (or set it to `0`) to restore normal DMA-BUF negotiation.
The wrapper does not enable `G_DEBUG=fatal-warnings`, because that setting is
intended for tests and turns expected user-facing capture failures into process
aborts.

`kasasa --diagnostics` prints a compact, shareable report containing the
application version, session variables, GStreamer version and
`gtk4paintablesink` availability, external capture tools, and layer-shell build
support. It does not open a window or start a capture.

When running listing or capture commands in a sandbox where the dconf database
is read-only, prefix the command with `GSETTINGS_BACKEND=memory`. Diagnostics
mode already skips preference initialization and does not need this workaround.

For an interactive GTK inspector, run the application with
`GTK_DEBUG=interactive` in the same command, for example:

```sh
GTK_DEBUG=interactive ./tools/run-debug -- --screencast
```

## Capturing a crash or hang

Start a debug session with `--gdb`, then use this command in gdb:

```gdb
set pagination off
run
thread apply all bt full
```

For an already-recorded crash, use the system coredump service:

```sh
coredumpctl list kasasa
coredumpctl gdb kasasa
```

When diagnosing a close or stop hang, capture two `thread apply all bt full`
backtraces a few seconds apart. The Wayland worker, GTK main loop, and any
GStreamer task threads should be included.

## Sanitizers and focused tests

Use a separate build directory so sanitizer flags cannot mix with a normal
build:

```sh
meson setup build-sanitize --buildtype=debug \
  -Db_sanitize=address,undefined
meson compile -C build-sanitize
ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
meson test -C build-sanitize --suite unit --print-errorlogs
```

Leak detection is disabled for the GTK suite because GLib, GTK, fontconfig,
and GPU drivers retain process-global allocations. Use Valgrind for focused
capture lifecycle checks:

```sh
valgrind --leak-check=full --track-fds=yes \
  --errors-for-leak-kinds=definite --error-exitcode=1 \
  build/tests/test-hyprland-capture
```

For data races in the capture worker, stream startup, and cancellation paths,
use a separate ThreadSanitizer build and run only the focused stream tests:

```sh
meson setup build-tsan --buildtype=debug -Db_sanitize=thread
meson compile -C build-tsan
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:history_size=7 \
  build-tsan/tests/test-window-query \
  -p /unit/hyprland-stream/connection-failure
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1:history_size=7 \
  build-tsan/tests/test-window-query \
  -p /unit/hyprland-stream/output-connection-failure
```

The CI TSan lane deliberately excludes GTK and image-loader suites so reports
focus on capture-thread synchronization rather than process-global framework
allocations.

Install `gcovr` and use a separate build directory to inspect test coverage:

```sh
meson setup build-coverage --buildtype=debug -Db_coverage=true
meson compile -C build-coverage
meson test -C build-coverage --suite unit --print-errorlogs
meson compile -C build-coverage coverage-html coverage-xml
```

The HTML report is written to
`build-coverage/meson-logs/coveragereport/index.html`; the XML report is
`build-coverage/meson-logs/coverage.xml`. CI uploads both as the
`kasasa-coverage` artifact. This is a visibility baseline rather than a hard
percentage gate; use it to confirm that new tests actually enter the capture
and cancellation paths they are intended to cover.

Region-capture tests use fake `slurp` and `grim` programs. Failures mentioning
an image-loader D-Bus sandbox or `Operation not permitted` are external loader
environment failures, not sanitizer diagnostics; rerun in the CI container
before investigating application memory.

## Real Hyprland capture

The regular GTK tests use Xvfb and do not exercise the Hyprland capture
protocol. On a real Hyprland session, enable the opt-in suite:

```sh
meson setup build-integration --buildtype=debug -Dintegration_tests=true
meson compile -C build-integration
meson test -C build-integration --suite integration --print-errorlogs
```

The integration binary creates a temporary GTK window, resolves its live
Hyprland client address, captures one real window frame, and then stops the
worker. The window test records whether the first frame used DMA-BUF or the
wl_shm fallback. It also captures the first frame from the first GDK monitor;
set `KASASA_INTEGRATION_OUTPUT=NAME` to select a different output. Run this
suite from the host Wayland session rather than a sandbox that cannot access
the Hyprland socket. Layer-shell is reported as skipped when the compositor
does not support layer-shell requests.

For issue reports, attach the command line, `G_MESSAGES_DEBUG=all` output, the
relevant GStreamer dot files, and the Meson test log when a test fails.

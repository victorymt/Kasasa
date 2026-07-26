# Contributing

## Building

Use the Flatpak version of [GNOME Builder](https://apps.gnome.org/Builder) to build and run the project from source:

1. Open Builder and press the "Clone Repository…" button
2. Paste a link to the repository in the "Repository URL" field:

   ```
   https://github.com/KelvinNovais/Kasasa
   ```

3. Press the "Clone repository…" button
4. Press confirm if asked aboout automatic installation of any dependencies, and wait for them to download
5. Press the play button in the header bar

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

Tests that require a real Wayland display managed by Hyprland are opt-in and are
not run by regular CI:

```sh
meson setup build-integration -Dintegration_tests=true
meson compile -C build-integration
meson test -C build-integration --suite integration --print-errorlogs
```

## Translating

1. Fork the repository and clone it
2. Take the `po/kasasa.pot` file and generate a new `.po` translation file from it; also add the new translation language to `po/LINGUAS`
3. Submit a PR with the changed files

_Note: you can use [Translation Editor](https://flathub.org/apps/org.gnome.Gtranslator) or another app to make the translations_

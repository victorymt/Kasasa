<img height="128" src="data/icons/hicolor/scalable/apps/io.github.kelvinnovais.Kasasa.svg" align="left"/> 

# Kasasa

Clip and pin what's important to a small floating window, so you don't have to switch between windows or workspaces repeatedly. 
The window can become miniaturized or have its opacity reduced, in order to do not block what's behind it.

Best used with:
 - "Always on Top" and/or "Always on Visible Workspace"
 - A keyboard shortcut

https://github.com/user-attachments/assets/eb98f2e0-d3cc-4461-bc84-25f438120b58

> [!NOTE]
> On GNOME, go to Settings → Keyboard → View and Customize Shortcuts → Custom Shortcuts.
> 
> There you can set a shortcut to call **`kasasa`**.

> [!NOTE]
> If using Hyprland, add the following rule:
> `windowrule = match:title Kasasa, no_blur on, border_size 0, no_shadow on, no_anim on`
>
> `no_anim on` is important for scroll-zoom: otherwise Hyprland smoothly
> animates the window border while the app has already changed the screenshot
> size, so the two look out of sync.
>
> Bind separate shortcuts for screenshot and screencast, for example:
>
> ```
> bind = SUPER, Y, exec, kasasa
> bind = SUPER SHIFT, Y, exec, kasasa --screencast
> ```
>
> `kasasa` starts with an interactive screenshot. `kasasa --screencast` (or
> `-c`) starts directly with the screencast picker. If Kasasa is already
> running, `--screencast` appends a new screencast to the existing window.
>
> On Hyprland you can also list windows and pin a specific one without the
> interactive picker (needs `hyprctl` and `grim`):
>
> ```
> kasasa --list-windows
> kasasa --list-windows --json
> kasasa --window=Alacritty
> kasasa --window=title:nvim
> kasasa --window=active
> kasasa --window=address:0x1234abcd
> ```
>
> Bare `--window=NAME` matches `class` first, then title substring. Multiple
> matches print candidates and exit with status 2.
>
> Live pin of a specific window (Hyprland toplevel-export, no Portal picker):
>
> ```
> kasasa --screencast --window=Alacritty
> kasasa --screencast --window=title:nvim
> kasasa --screencast --window=active
> ```
>
> Hyprland-native monitor capture is also available. It bypasses Portal and
> captures the named output directly:
>
> ```
> kasasa --list-monitors
> kasasa --list-monitors --json
> kasasa --screencast --monitor=active
> kasasa --screencast --monitor=DP-1
> ```
>
> `--monitor` is for live capture only, so it requires `--screencast` and
> cannot be combined with `--window`.

## Screencast

Start a live pin from the toolbar, or launch Kasasa with
`kasasa --screencast` (`-c`). The desktop portal lets you choose either an
entire screen or an individual window.

On Hyprland, **More actions → Active monitor (Hyprland)** captures the active
monitor through Wayland's native image-copy-capture protocol without opening
the Portal picker. The regular **Screencast** action remains Portal-based, so
it can still interactively select a screen or a window.

- Live previews default to a 30 FPS limit. Choose any limit from 1 to 120 FPS
  under **Preferences → Screencast → Frame rate limit**; the new value applies
  when the next screencast starts, for both Portal and Hyprland-native capture.
  Kasasa prefers the GPU-accelerated GL pipeline when it is available and
  falls back to CPU rendering when necessary.
- Capturing a screen that also contains the Kasasa window creates a recursive
  preview and requires extra rendering work. Select a window, or move Kasasa
  off the captured screen, when recursion is not wanted.
- Move the pointer over the live pin to reveal its bottom-right controls, then
  click the stop icon (tooltip: **Stop screencast**) to finish it and release
  the capture session.
- Only one screencast can be active at a time. Finish the current stream before
  starting another one.

> [!IMPORTANT]
> On GNOME versions < 46, a dialog will appear to set up and take the screenshot,
> instead of directly using the GNOME's screenshoter; this may be inconvenient. 

## Building

Kasasa uses Meson. Install a C compiler, Meson, Ninja, and the development
packages for GTK 4, Libadwaita, libportal, GStreamer, JSON-GLib, and Wayland,
plus `wayland-protocols` (including the staging image-copy-capture XML files),
then run:

```sh
meson setup build --buildtype=release
meson compile -C build
./build/src/kasasa
```

To install the compiled application:

```sh
meson install -C build
```

## Language

Open **Menu → Preferences → General → Language** to choose **System
default**, **English**, or **简体中文**. The selected language is saved
immediately and takes effect after Kasasa is fully closed and started again.


## Screenshots

<div align="center">
  <img src="https://github.com/KelvinNovais/Kasasa/blob/main/screenshots/01.png" />
  <img src="https://github.com/KelvinNovais/Kasasa/blob/main/screenshots/05.png" />
</div>
<div align="center">
  <img src="https://github.com/KelvinNovais/Kasasa/blob/main/screenshots/04.png" />
</div>

---

## Fork

This repository is a fork of the original project:

- Upstream: [KelvinNovais/Kasasa](https://github.com/KelvinNovais/Kasasa)
- This fork: [victorymt/Kasasa](https://github.com/victorymt/Kasasa)

Upstream remains the canonical project for general releases and packaging.
This fork focuses on Hyprland-oriented capture and CLI workflows.

## License

Kasasa is free software licensed under the
[GNU General Public License v3.0 or later](COPYING) (`GPL-3.0-or-later`).

You should have received a copy of the GNU General Public License along with
this program (see [`COPYING`](COPYING)). If not, see
<https://www.gnu.org/licenses/>.

Copyright remains with the original author and subsequent contributors:

- © 2024–2026 Kelvin Ribeiro Novais
- © 2026 victorymt

The Wayland protocol description in
`protocols/hyprland-toplevel-export-v1.xml` is copyrighted by its original
authors and keeps its own license terms as distributed upstream.

---

_¹ "Kasasa" is an approximation to "قصاصة", an Arabic term meaning a scrap of paper torn from a book, a magazine or a newspaper._

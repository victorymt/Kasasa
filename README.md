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
> `kasasa` opens a Hyprland window picker for a screenshot.
> `kasasa --screencast` (or `-c`) opens the same picker for a live preview. If
> Kasasa is already running, `--screencast` appends a new live preview to the
> existing window.
>
> On Hyprland you can also list windows and pin a specific one without the
> interactive picker (needs `hyprctl` and a Hyprland Wayland session):
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
> Live pin of a specific window without opening the interactive picker:
>
> ```
> kasasa --screencast --window=Alacritty
> kasasa --screencast --window=title:nvim
> kasasa --screencast --window=active
> ```
>
> Window previews use a three-buffer GBM pool and import each
> DMA-BUF directly as a `GdkTexture`; GStreamer and intermediate GL conversion
> are bypassed on this path. Rotation and Y inversion are applied by the GTK
> snapshot transform. Kasasa falls back to `wl_shm` when allocation or GTK
> import is unavailable.
>
> Native monitor capture is also available and captures the named output
> directly:
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
`kasasa --screencast` (`-c`). Kasasa lists capturable Hyprland windows itself
and streams the selected window through the native toplevel-export protocol.

On Hyprland, **More actions → Active monitor (Hyprland)** captures the active
monitor through Wayland's native image-copy-capture protocol. Window and
monitor capture do not use the desktop Portal.

- Live previews default to a 30 FPS limit. Choose any limit from 1 to 120 FPS
  under **Preferences → Screencast → Frame rate limit**; the new value applies
  when the next screencast starts. Kasasa prefers direct DMA-BUF import and
  falls back to shared-memory frames when necessary.
- Move the pointer over the live pin to reveal its bottom-right controls, then
  click the stop icon (tooltip: **Stop screencast**) to finish it and release
  the capture session.
- Only one screencast can be active at a time. Finish the current stream before
  starting another one.

> [!IMPORTANT]
> Window selection and capture require Hyprland, `hyprctl`, and the Hyprland
> toplevel-export Wayland protocol.

## Building

Kasasa uses Meson. Install a C compiler, Meson, Ninja, and the development
packages for GTK 4.14+, Libadwaita, libportal, GStreamer, JSON-GLib, and Wayland,
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

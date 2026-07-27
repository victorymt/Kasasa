<img height="128" src="data/icons/hicolor/scalable/apps/io.github.kelvinnovais.Kasasa.svg" align="left"/> 

# Kasasa

<p align="center">
 <!--
  <a href="https://github.com/KelvinNovais/Kasasa/actions/workflows/flatpak.yml">
    <img src="https://img.shields.io/github/actions/workflow/status/KelvinNovais/Kasasa/flatpak.yml?logo=flatpak&logoColor=fff&labelColor=22d841&color=f9f28f"/>
 </a>
  <a href="https://github.com/KelvinNovais/Kasasa/releases/latest">
    <img src="https://img.shields.io/github/v/release/KelvinNovais/Kasasa?logo=github&logoColor=fff&labelColor=22d841&color=f9f28f"/>
  </a>
 -->
  <a href="https://flathub.org/apps/io.github.kelvinnovais.Kasasa">
    <img src="https://img.shields.io/flathub/downloads/io.github.kelvinnovais.Kasasa?logo=flathub&logoColor=fff&labelColor=22d841&color=white"/>
  </a>
</p>

Clip and pin what's important to a small floating window, so you don't have to switch between windows or workspaces repeatedly. 
The window can become miniaturized or have its opacity reduced, in order to do not block what's behind it.

Best used with:
 - "Always on Top" and/or "Always on Visible Workspace"
 - A keyboard shortcut

https://github.com/user-attachments/assets/eb98f2e0-d3cc-4461-bc84-25f438120b58

> [!NOTE]
> On GNOME, go to Settings → Keyboard → View and Customize Shortcuts → Custom Shortcuts.
> 
> There you can set a shortcut to call **`flatpak run io.github.kelvinnovais.Kasasa`**

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

> [!IMPORTANT]
> On GNOME versions < 46, a dialog will appear to set up and take the screenshot,
> instead of directly using the GNOME's screenshoter; this may be inconvenient. 

## Installation

[<img width="240" alt="Download on Flathub" src="https://flathub.org/api/badge?svg&locale=en"/>](https://flathub.org/apps/io.github.kelvinnovais.Kasasa)


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

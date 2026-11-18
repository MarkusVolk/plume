# Plume

A plain text editor for Wayland in the style of the Noctalia desktop: tabs, line numbers, soft
wrap, search and replace, sorting lines, and a session that comes back as you left it. It draws
with Noctalia's UI library, without GTK or Qt, and follows the Noctalia colour scheme.

Plume is young. See [CREDITS.md](CREDITS.md) for what it builds on.

## Build

```sh
meson setup build --buildtype=release
meson compile -C build
PLUME_ASSETS_DIR=$PWD/lib/assets ./build/src/plume file.txt
```

Dependencies: a C++23 compiler, Meson, wayland-scanner, wayland-protocols, wayland-client,
wayland-cursor, wayland-egl, EGL and GLES, GLib/GIO, Pango, Cairo, HarfBuzz, FreeType, fontconfig,
xkbcommon, tomlplusplus, nlohmann_json and the stb headers.

## Keys

| Key | Action |
| --- | --- |
| Ctrl+T, Ctrl+N | New tab |
| Ctrl+W | Close tab |
| Ctrl+S | Save |
| Ctrl+Plus, Ctrl+Minus, Ctrl+0 | Zoom |
| F7 | Line numbers |
| F8 | Wrap lines |
| F9 | Sort lines (the selected ones, or all) |

## Colours

Plume reads `~/.config/plume/colors.toml` and reloads it when it changes. Noctalia writes it from
the template in `assets/templates/colors.toml`:

```toml
[theme.templates.user.plume]
input_path  = "/usr/share/plume/templates/colors.toml"
output_path = "$XDG_CONFIG_HOME/plume/colors.toml"
```

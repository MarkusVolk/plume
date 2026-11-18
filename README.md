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
| Ctrl+O | Open (the desktop's file dialog) |
| Ctrl+S, Ctrl+Shift+S | Save, Save As |
| Ctrl+W | Close tab |
| Ctrl+Tab, Ctrl+Shift+Tab, Alt+1…9 | Switch tabs |
| Ctrl+F, Ctrl+H | Find, Find and Replace |
| Enter, Shift+Enter, F3, Shift+F3, Ctrl+G | Next and previous match |
| Ctrl+Plus, Ctrl+Minus, Ctrl+0 | Zoom |
| F7 | Line numbers |
| F8 | Wrap lines |
| F9, Shift+F9 | Sort lines ascending, descending (the selected ones, or all) |
| F10 | Menu |
| Ctrl+Q | Quit; unsaved texts come back on the next start |

Replacement text of a regular-expression search may refer to groups with `\1` or `\g<name>`.

## Files

Text is read as UTF-8, UTF-16 with a byte order mark, or else Windows-1252, and saved the way it
was found, including CR LF line ends. Binary files open read-only: their text runs stay readable
and searchable, other bytes show as dots.

Plume registers the type `text/x-bitbake` for `.bb`, `.bbappend` and `.bbclass` files, so a
file manager can open recipes with it.

Unsaved texts are kept in `~/.local/share/plume/drafts` and the open tabs in
`~/.local/state/plume/session.toml`, so quitting never asks and never loses anything. Closing a
single tab with unsaved changes does ask.

## Syntax highlighting

Plume colors code with [tree-sitter](https://tree-sitter.github.io). It brings no grammars of its
own: for a language it needs the parser as `<language>.so` (exporting `tree_sitter_<language>`)
and the query `<language>/highlights.scm`, optionally `injections.scm` for embedded languages
such as shell in BitBake recipes or code blocks in Markdown. They are looked for in

- parsers: `$PLUME_PARSER_PATH`, `~/.local/share/plume/parsers`, `/usr/lib/tree-sitter`,
  `/usr/lib/nvim/parser` (shared with neovim)
- queries: `$PLUME_QUERY_PATH`, `~/.local/share/plume/queries`, `/usr/share/tree-sitter/queries`

A file without a grammar is plain text; texts over 8 MB are not parsed. The colors come from the
`[syntax]` table of `colors.toml`, which the template fills from the terminal palette.

## Settings

Defaults go into `~/.config/plume/config.toml`; what the menu changes is written to
`settings.toml` next to it.

```toml
[editor]
font_family = "monospace"
font_size = 13
tab_width = 8
insert_spaces = false
line_numbers = true
wrap = true
highlight_current_line = true
auto_indent = true
syntax_highlighting = true
```

## Actions over D-Bus

A second `plume file` opens the file in the running window. The window also takes the actions of
its menu through `org.freedesktop.Application.ActivateAction` on `de.flk.Plume`, for example:

```sh
busctl --user call de.flk.Plume /de/flk/Plume org.freedesktop.Application \
  ActivateAction 'sava{sv}' find 1 s 'TODO' 0
```

Actions: `new-tab`, `open`, `save`, `save-as`, `close-tab`, `quit`, `undo`, `redo`, `cut`, `copy`,
`paste`, `select-all`, `find`, `replace`, `sort-lines`, `sort-lines-descending`,
`sort-lines-unique`, `line-numbers`, `wrap`, `highlight-line`, `zoom-in`, `zoom-out`, `zoom-reset`,
`syntax-highlighting`, `menu`, `insert-text <text>`, `go-to-line <number>`, `select-lines <first>-<last>`, `select-range <anchor>-<cursor>` (bytes).

## Colours

Plume reads `~/.config/plume/colors.toml` and reloads it when it changes. Noctalia writes it from
the template in `assets/templates/colors.toml`:

```toml
[theme.templates.user.plume]
input_path  = "/usr/share/plume/templates/colors.toml"
output_path = "$XDG_CONFIG_HOME/plume/colors.toml"
```

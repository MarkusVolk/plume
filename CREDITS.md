# Credits

Plume is built on the UI library of [Noctalia](https://github.com/noctalia-dev/noctalia) and
borrows application plumbing from [Luciole](https://github.com/noctalia-dev/luciole). Both are
MIT licensed, Copyright (c) 2026 noctalia-dev.

- `lib/` is an unchanged copy of Noctalia's `lib/` (without `third_party/`), refreshed with
  `tools/sync-lib.sh`. Changes to it belong upstream.
- From Luciole, adapted: `src/app/main_loop.*`, `src/app/palette_watcher.*`, `src/event_loop/`,
  `src/fs/directory_watch.*`, `src/fs/gio_ptr.h`, `src/fs/xdg_dirs.*`,
  `src/platform/desktop_integration.*`, `src/platform/launch_activation.*`,
  `src/ui/palette_file.*`, and the shape of the tab strip in `src/app/editor_window.cpp`.
- `lib/assets/fonts/` carries the Tabler icon font; its license is
  `lib/assets/fonts/tabler-icons-license.txt`.

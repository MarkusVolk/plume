app_sources = files(
  'app/application.cpp',
  'app/editor_window.cpp',
  'app/main_loop.cpp',
  'app/palette_watcher.cpp',
  'document/file_io.cpp',
  'event_loop/glib_poll_source.cpp',
  'fs/directory_watch.cpp',
  'fs/xdg_dirs.cpp',
  'main.cpp',
  'platform/clipboard.cpp',
  'platform/desktop_integration.cpp',
  'platform/launch_activation.cpp',
  'ui/palette_file.cpp',
)

executable('plume',
  app_sources + protocol_sources + protocol_headers + [build_info],
  include_directories: src_inc,
  dependencies: [
    view_dep, gio, gio_unix, tomlplusplus,
    dependency('wayland-client'), dependency('wayland-cursor'), dependency('threads'),
  ],
  install: true,
)

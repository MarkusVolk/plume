app_sources = files(
  'app/application.cpp',
  'app/editor_window.cpp',
  'app/instance_service.cpp',
  'app/main_loop.cpp',
  'app/palette_watcher.cpp',
  'app/settings.cpp',
  'event_loop/glib_poll_source.cpp',
  'fs/directory_watch.cpp',
  'main.cpp',
  'platform/clipboard.cpp',
  'platform/desktop_integration.cpp',
  'platform/file_portal.cpp',
  'platform/launch_activation.cpp',
  'ui/confirm_dialog.cpp',
  'ui/palette_file.cpp',
  'ui/search_bar.cpp',
  'ui/tooltip_presenter.cpp',
)

executable('plume',
  app_sources + protocol_sources + protocol_headers + [build_info],
  include_directories: src_inc,
  dependencies: [
    view_dep, files_dep, gio, gio_unix, tomlplusplus,
    dependency('wayland-client'), dependency('wayland-cursor'), dependency('threads'),
  ],
  install: true,
)

project('plume', ['c', 'cpp'],
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=3',
    'b_ndebug=if-release',
  ],
)

cpp = meson.get_compiler('cpp')

wayland_protocols = dependency('wayland-protocols', version: '>=1.49')
scanner = find_program('wayland-scanner')
protocol_root = wayland_protocols.get_variable('pkgdatadir')
protocol_sources = []
protocol_headers = []
foreach protocol : [
  ['xdg-decoration-unstable-v1', 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'],
  ['xdg-foreign-unstable-v2', 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml'],
]
  header = custom_target(protocol[0] + '-header',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  source = custom_target(protocol[0] + '-source',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    depends: header,
  )
  protocol_sources += source
  protocol_headers += header
endforeach

glib = dependency('glib-2.0', version: '>=2.76')
gio = dependency('gio-2.0', version: '>=2.76')
gio_unix = dependency('gio-unix-2.0', version: '>=2.76')
pango = dependency('pango', version: '>=1.50')
tomlplusplus = dependency('tomlplusplus', version: '>=3.4')

if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

if get_option('buildtype') == 'release'
  add_project_arguments('-ffunction-sections', '-fdata-sections', language: 'cpp')
  add_project_link_arguments('-Wl,--gc-sections', '-Wl,--as-needed', language: 'cpp')
else
  add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
endif

subdir('lib')

build_config = configuration_data()
build_config.set_quoted('PLUME_VERSION', meson.project_version())
build_config.set_quoted('PLUME_BUILD_REVISION', get_option('build_revision'))
build_config.set_quoted('PLUME_INSTALL_ASSETSDIR', get_option('prefix') / get_option('datadir') / 'plume/assets')
build_info = configure_file(
  input: 'src/app/build_info.h.in',
  output: 'plume_build_info.h',
  configuration: build_config,
)
src_inc = include_directories('src')

# The text model knows nothing about the toolkit: lib/ headers do not compile into it.
text_lib = static_library('plume-text',
  files(
    'src/text/buffer.cpp',
    'src/text/document.cpp',
    'src/text/operations.cpp',
    'src/text/unicode.cpp',
  ),
  include_directories: src_inc,
  dependencies: [glib, pango],
)
text_dep = declare_dependency(link_with: text_lib, include_directories: src_inc, dependencies: [glib, pango])

# Reading and writing files, drafts and the session.
files_lib = static_library('plume-files',
  files(
    'src/document/file_io.cpp',
    'src/document/session.cpp',
    'src/fs/xdg_dirs.cpp',
  ),
  include_directories: src_inc,
  dependencies: [text_dep, gio, tomlplusplus],
)
files_dep = declare_dependency(link_with: files_lib, include_directories: src_inc, dependencies: [text_dep, gio, tomlplusplus])

# Syntax trees and the colors that follow from them. Parsers are loaded at run time.
tree_sitter = dependency('tree-sitter', version: '>=0.25')
syntax_lib = static_library('plume-syntax',
  files(
    'src/syntax/grammar.cpp',
    'src/syntax/highlighter.cpp',
    'src/syntax/token_style.cpp',
  ),
  include_directories: src_inc,
  dependencies: [text_dep, tree_sitter, cpp.find_library('dl', required: false)],
)
syntax_dep = declare_dependency(
  link_with: syntax_lib, include_directories: src_inc,
  dependencies: [text_dep, tree_sitter, cpp.find_library('dl', required: false)],
)

# The editing surface, without the application around it: the view tests drive it headless.
view_lib = static_library('plume-view',
  files('src/ui/text_view.cpp'),
  include_directories: src_inc,
  dependencies: [text_dep, noctalia_ui_dep],
)
view_dep = declare_dependency(link_with: view_lib, include_directories: src_inc, dependencies: [text_dep, noctalia_ui_dep])

subdir('src')

tests_opt = get_option('tests')
if tests_opt.enabled() or (tests_opt.auto() and get_option('buildtype') == 'debug')
  subdir('tests')
endif

install_subdir(noctalia_ui_assets_dir / 'fonts', install_dir: get_option('datadir') / 'plume/assets')
install_data('assets/templates/colors.toml', install_dir: get_option('datadir') / 'plume/templates')
install_data('assets/desktop/de.flk.Plume.desktop', install_dir: get_option('datadir') / 'applications')
install_data('assets/icons/de.flk.Plume.svg', install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
install_data('assets/mime/de.flk.Plume.xml', install_dir: get_option('datadir') / 'mime/packages')
install_data('LICENSE', install_dir: get_option('datadir') / 'licenses/plume')

project(
  'y720-control',
  'c',
  version: '0.1.0',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=false',
  ],
)

gnome = import('gnome')

gtk_dep = dependency('gtk4', version: '>=4.16')
adw_dep = dependency('libadwaita-1', version: '>=1.5')
json_dep = dependency('json-glib-1.0', version: '>=1.6')
pixbuf_dep = dependency('gdk-pixbuf-2.0')
gio_dep = dependency('gio-2.0')
x11_dep = dependency('x11', required: false)
math_dep = meson.get_compiler('c').find_library('m', required: false)

resources = gnome.compile_resources(
  'y720-control-resources',
  'data/resources.gresource.xml',
  source_dir: 'data',
  c_name: 'y720_control',
)

sources = files(
  'src/main.c',
  'src/y720-app.c',
  'src/y720-window.c',
  'src/y720-cli.c',
  'src/y720-models.c',
  'src/y720-settings.c',
  'src/y720-tray.c',
  'src/y720-hotkeys.c',
  'src/y720-fan-page.c',
  'src/y720-fan-curve.c',
  'src/y720-temp.c',
  'src/y720-backlight-page.c',
  'src/y720-keyboard.c',
  'src/y720-profiles.c',
  'src/y720-settings-dialog.c',
)

cargs = ['-D_GNU_SOURCE', '-DY720_VERSION="' + meson.project_version() + '"']
deps = [gtk_dep, adw_dep, json_dep, gio_dep, pixbuf_dep, math_dep]
if x11_dep.found()
  cargs += ['-DHAVE_X11=1']
  deps += x11_dep
endif

executable(
  'y720-control',
  sources,
  resources,
  include_directories: include_directories('src'),
  dependencies: deps,
  c_args: cargs,
  install: true,
)

install_data('data/org.y720.Control.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('data/icons/org.y720.Control.png',
  install_dir: get_option('datadir') / 'icons/hicolor/256x256/apps')
install_data('data/icons/org.y720.Control.png',
  install_dir: get_option('datadir') / 'icons/hicolor/128x128/apps')
install_data('data/org.y720.Control.metainfo.xml',
  install_dir: get_option('datadir') / 'metainfo')
policy = configure_file(
  input: 'data/org.y720.Control.policy.in',
  output: 'org.y720.Control.policy',
  configuration: {'BINDIR': get_option('prefix') / get_option('bindir')})
install_data(policy, install_dir: '/usr/share/polkit-1/actions')

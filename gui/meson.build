project(
  'y720-control',
  'c',
  version: files('../VERSION')[0],
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=3',
    'werror=false',
  ],
)

gnome = import('gnome')

gtk_dep = dependency('gtk4', version: '>=4.16')
adw_dep = dependency('libadwaita-1', version: '>=1.7')
json_dep = dependency('json-glib-1.0', version: '>=1.6')
pixbuf_dep = dependency('gdk-pixbuf-2.0')
gio_dep = dependency('gio-2.0')
x11_dep = dependency('x11', required: get_option('x11'))
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
  'src/y720-portal-hotkeys.c',
  'src/y720-evdev.c',
  'src/y720-i18n.c',
  'src/y720-fan-page.c',
  'src/y720-fan-curve.c',
  'src/y720-temp.c',
  'src/y720-backlight-page.c',
  'src/y720-keyboard.c',
  'src/y720-profiles.c',
  'src/y720-settings-dialog.c',
)

cargs = ['-D_GNU_SOURCE', '-DY720_VERSION="' + meson.project_version() + '"',
         '-DY720_BINDIR="' + (get_option('prefix') / get_option('bindir')) + '"']
deps = [gtk_dep, adw_dep, json_dep, gio_dep, pixbuf_dep, math_dep]
if x11_dep.found()
  cargs += ['-DHAVE_X11=1']
  deps += x11_dep
endif

app_binary = executable(
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
python = import('python').find_installation('python3')
sudoers = configure_file(
  output: 'y720-control.sudoers',
  command: [python, files('tools/make-sudoers.py'),
            get_option('prefix') / get_option('bindir'), '@OUTPUT@'])
install_data('data/70-y720-control.rules',
  install_dir: '/usr/lib/udev/rules.d')
install_data(sudoers,
  install_dir: '/etc/sudoers.d',
  install_mode: 'r--r-----',
  rename: 'y720-control')
install_data('../LICENSE', rename: 'copyright',
  install_dir: get_option('datadir') / 'doc/y720-control')
install_data('../LICENSES/MIT.txt', '../LICENSES/GPL-2.0.txt',
  install_dir: get_option('datadir') / 'doc/y720-control/LICENSES')

cli_fixture = executable('y720-fan-fixture', 'tests/cli-fixture.c', c_args: cargs)
core_test = executable('test-gui-core',
  ['tests/test-core.c', 'src/y720-models.c', 'src/y720-temp.c', 'src/y720-cli.c', 'src/y720-i18n.c'],
  include_directories: include_directories('src'), c_args: cargs,
  dependencies: deps)
test('GUI protocol, lifetime and curve policy', core_test, args: [cli_fixture.full_path()],
     depends: cli_fixture, timeout: 30)
evdev_test = executable('test-gui-evdev', ['tests/test-evdev.c', 'src/y720-i18n.c'],
  c_args: cargs, dependencies: deps)
test('Keypad layers, repeat, loss and disconnect', evdev_test)
settings_test = executable('test-gui-settings',
  ['tests/test-settings.c', 'src/y720-settings.c', 'src/y720-models.c', 'src/y720-i18n.c'],
  include_directories: include_directories('src'), c_args: cargs, dependencies: deps)
test('Shortcut defaults, persistence and validation', settings_test)
portal_test = executable('test-portal-hotkeys',
  ['tests/test-portal-hotkeys.c', 'src/y720-portal-hotkeys.c', 'src/y720-i18n.c'],
  include_directories: include_directories('src'), c_args: cargs, dependencies: deps)
test('Wayland portal authorization, activation and cancellation', portal_test, timeout: 30)
shortcut_dialog_test = executable('test-shortcut-dialog',
  ['tests/test-shortcut-dialog.c', 'src/y720-settings.c', 'src/y720-models.c', 'src/y720-i18n.c'],
  include_directories: include_directories('src'), c_args: cargs, dependencies: deps)
test('Shortcut recording, permission denial and cancellation', find_program('sh'),
  args: [files('tests/run-headless.sh'), shortcut_dialog_test.full_path()],
  depends: shortcut_dialog_test, timeout: 20)
widgets_test = executable('test-gui-widgets',
  ['tests/test-widgets.c', 'src/y720-backlight-page.c',
   'src/y720-fan-curve.c', 'src/y720-keyboard.c', 'src/y720-models.c',
   'src/y720-profiles.c', 'src/y720-temp.c', 'src/y720-cli.c', 'src/y720-i18n.c'],
  include_directories: include_directories('src'), c_args: cargs, dependencies: deps)
test('GTK widget lifetime and in-flight cancellation', find_program('sh'),
  args: [files('tests/run-headless.sh'), widgets_test.full_path(), cli_fixture.full_path()],
  depends: [widgets_test, cli_fixture], timeout: 45)
test('Tray quit via Event and EventGroup', find_program('sh'),
  args: [files('tests/run-headless.sh'), python, files('tests/test-tray.py'),
         app_binary.full_path(), cli_fixture.full_path()],
  depends: [app_binary, cli_fixture], timeout: 35)

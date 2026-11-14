project(
  'videoshot',
  'c',
  version: '0.1.0-beta1',
  license: 'GPLv2+',
  meson_version: '>= 0.50.0',
  default_options: [
    'buildtype=debugoptimized',
    'warning_level=1',
    'c_std=c99',
  ]
)

add_project_arguments([
  '-D_POSIX_C_SOURCE=200809L',
  '-D_XOPEN_SOURCE=700',
  '-D_DEFAULT_SOURCE'
], language: 'c')

cc = meson.get_compiler('c')

videoshot_prefix = get_option('prefix')
videoshot_bindir = join_paths(videoshot_prefix, get_option('bindir'))
videoshot_datadir = join_paths(videoshot_prefix, get_option('datadir'))
videoshot_libexecdir = join_paths(videoshot_prefix, get_option('libexecdir'))
videoshot_localedir = join_paths(videoshot_datadir, 'locale')
videoshot_appsdir = join_paths(videoshot_datadir, 'applications')
videoshot_appdatadir = join_paths(videoshot_datadir, 'metainfo')
videoshot_servicesdir = join_paths(videoshot_datadir, 'dbus-1', 'services')
videoshot_schemadir = join_paths(videoshot_datadir, 'glib-2.0', 'schemas')

glib_req_version = '>= 2.35.1'
gtk_req_version = '>= 3.12.0'
libhandy_req_version = '>= 1.5.0'

mathlib_dep = cc.find_library('m', required: false)
x11_dep = dependency('x11', required: get_option ('x11'))
xext_dep = dependency('xext', required: get_option ('x11'))
glib_dep = dependency('glib-2.0', version: glib_req_version)
gtk_dep = dependency('gtk+-3.0', version: gtk_req_version)
libhandy_dep = dependency('libhandy-1', version: libhandy_req_version)

config_h = configuration_data()
config_h.set_quoted('VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', meson.project_name())

has_x11 = x11_dep.found() and xext_dep.found()
if has_x11
  config_h.set('HAVE_X11', 1)

  if cc.has_header('X11/extensions/shape.h')
    config_h.set('HAVE_X11_EXTENSIONS_SHAPE_H', 1)
  endif
endif

configure_file(output: 'config.h', configuration: config_h)

root_inc = include_directories('.')

i18n = import('i18n')

subdir('data')
subdir('src')
subdir('po')

meson.add_install_script('build-aux/postinstall.py')

sources = [
  'videoshot.c',

  'cheese-flash.c',

  'screenshot-application.c',
  'screenshot-area-selection.c',
  'screenshot-backend.c',
  'screenshot-backend-shell.c',
  'screenshot-backend-x11.c',
  'screenshot-config.c',
  'screenshot-dialog.c',
  'screenshot-filename-builder.c',
  'screenshot-interactive-dialog.c',
  'screenshot-portal.c',
  'screenshot-preferences.c',
  'screenshot-utils.c',
  'screenshot-video.c',
]

include_directories = [
  root_inc,
  include_directories('.')
]

dependencies = [
  mathlib_dep,
  x11_dep,
  xext_dep,
  glib_dep,
  gio_unix_dep,
  gtk_dep,
  libhandy_dep
]

c_args = [
  '-DLOCALEDIR="@0@"'.format(videoshot_localedir),
  '-DLIBEXECDIR="@0@"'.format(videoshot_libexecdir),
]

executable(
  'videoshot',
  sources + resources,
  include_directories: include_directories,
  dependencies: dependencies,
  c_args: c_args,
  install: true
)

# Reads the frames of a video off the X screen
if has_x11
  executable(
    'videoshot-grab',
    'videoshot-grab.c',
    dependencies: [x11_dep, xext_dep, xfixes_dep],
    install: true,
    install_dir: videoshot_libexecdir
  )
endif

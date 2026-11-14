gnome = import('gnome')

desktop_file = i18n.merge_file(
  type: 'desktop',
  input: 'com.royake.Videoshot.desktop.in',
  output: 'com.royake.Videoshot.desktop',
  po_dir: join_paths(meson.current_source_dir(), '../po'),
  install: true,
  install_dir: videoshot_appsdir
)

# Validate Desktop file
desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test(
    'validate-desktop',
    desktop_file_validate,
    args: [
      desktop_file.full_path()
    ]
  )
endif

metainfo_file = i18n.merge_file(
  input: 'com.royake.Videoshot.metainfo.xml.in',
  output: 'com.royake.Videoshot.metainfo.xml',
  po_dir: join_paths(meson.current_source_dir(), '../po'),
  install: true,
  install_dir: videoshot_appdatadir
)

# Validate metainfo
appstream_util = find_program('appstream-util', required: false)
if appstream_util.found()
  test(
    'validate-metainfo',
    appstream_util,
    args: [
      'validate',
      '--nonet',
      metainfo_file.full_path()
    ]
  )
endif

service_conf = configuration_data()
service_conf.set('bindir', videoshot_bindir)
configure_file(
  input: 'com.royake.Videoshot.service.in',
  output: 'com.royake.Videoshot.service',
  configuration: service_conf,
  install: true,
  install_dir: videoshot_servicesdir
)

install_data(
  'com.royake.videoshot.gschema.xml',
  install_dir: videoshot_schemadir
)

glib_compile_schemas = find_program('glib-compile-schemas', required: false)
# Validata GSchema
if glib_compile_schemas.found()
  test(
    'validate-gschema',
    glib_compile_schemas,
    args: [
      '--strict',
      '--dry-run',
      meson.current_source_dir()
    ]
  )
endif

resources = gnome.compile_resources(
  'screenshot-resources',
  'com.royake.Videoshot.gresource.xml',
  source_dir: '.',
  c_name: 'screenshot'
)

install_data(
  'videoshot.1',
  install_dir: join_paths(get_option('mandir'), 'man1')
)

subdir('icons')

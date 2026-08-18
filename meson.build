project('kasasa', 'c',
          version: '1.2.0',
    meson_version: '>= 0.62.0',
  default_options: [ 'warning_level=2', 'werror=false', 'c_std=gnu11', ],
)

gtk_layer_shell_dep = dependency('gtk4-layer-shell-0', required: false)

i18n = import('i18n')
gnome = import('gnome')
cc = meson.get_compiler('c')

config_h = configuration_data()
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', 'kasasa')
config_h.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
configure_file(output: 'config.h', configuration: config_h)
add_project_arguments(['-I' + meson.project_build_root()], language: 'c')

project_c_args = []
test_c_args = [
  '-Wcast-align',
  '-Wdeclaration-after-statement',
  '-Werror=address',
  '-Werror=array-bounds',
  '-Werror=empty-body',
  '-Werror=implicit',
  '-Werror=implicit-function-declaration',
  '-Werror=incompatible-pointer-types',
  '-Werror=init-self',
  '-Werror=int-conversion',
  '-Werror=int-to-pointer-cast',
  '-Werror=main',
  '-Werror=misleading-indentation',
  '-Werror=missing-braces',
  '-Werror=missing-include-dirs',
  '-Werror=nonnull',
  '-Werror=overflow',
  '-Werror=parenthesis',
  '-Werror=pointer-arith',
  '-Werror=pointer-to-int-cast',
  '-Werror=redundant-decls',
  '-Werror=return-type',
  '-Werror=sequence-point',
  '-Werror=shadow',
  '-Werror=strict-prototypes',
  '-Werror=trigraphs',
  '-Werror=undef',
  '-Werror=write-strings',
  '-Wformat-nonliteral',
  '-Wignored-qualifiers',
  '-Wimplicit-function-declaration',
  '-Wlogical-op',
  '-Wmissing-declarations',
  '-Wmissing-format-attribute',
  '-Wmissing-noreturn',
  '-Wnested-externs',
  '-Wno-cast-function-type',
  '-Wno-dangling-pointer',
  '-Wno-missing-field-initializers',
  '-Wno-sign-compare',
  '-Wno-unused-parameter',
  '-Wold-style-definition',
  '-Wswitch-default',
  '-Wswitch-enum',
  '-Wuninitialized',
  '-Wunused',
  '-fno-strict-aliasing',
  ['-Werror=format-security', '-Werror=format=2'],
]
if get_option('buildtype') != 'plain'
  test_c_args += '-fstack-protector-strong'
endif
foreach arg: test_c_args
  if cc.has_multi_arguments(arg)
    project_c_args += arg
  endif
endforeach
add_project_arguments(project_c_args, language: 'c')

kasasa_query_lib = static_library(
  'kasasa-query',
  [
    'src/kasasa-hyprctl.c',
    'src/kasasa-window-model.c',
    'src/kasasa-window-resolver.c',
    'src/kasasa-window-query.c',
  ],
  include_directories: include_directories('src'),
  dependencies: [
    dependency('glib-2.0'),
    dependency('json-glib-1.0'),
  ],
)

kasasa_cli_lib = static_library(
  'kasasa-cli',
  'src/kasasa-cli.c',
  include_directories: include_directories('src'),
  dependencies: dependency('glib-2.0'),
  link_with: kasasa_query_lib,
)

kasasa_core_lib = static_library(
  'kasasa-core',
  [
    'src/kasasa-language.c',
    'src/kasasa-source.c',
    'src/kasasa-zoom.c',
    'src/kasasa-window-layout.c',
  ],
  include_directories: include_directories('src'),
  dependencies: [
    dependency('gio-2.0'),
    cc.find_library('m', required: true),
  ],
)

subdir('data')
subdir('src')
subdir('po')
subdir('tests')

gnome.post_install(
     glib_compile_schemas: true,
    gtk_update_icon_cache: true,
  update_desktop_database: true,
)

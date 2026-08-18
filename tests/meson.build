glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gtk_dep = dependency('gtk4', version: '>= 4.14')
adwaita_dep = dependency('libadwaita-1', version: '>= 1.7.4')
gstreamer_dep = dependency('gstreamer-1.0')
gstreamer_app_dep = dependency('gstreamer-app-1.0')
json_glib_dep = dependency('json-glib-1.0')
gbm_dep = dependency('gbm')
libdrm_dep = dependency('libdrm')
wayland_client_dep = dependency('wayland-client')
wayland_protocols_dep = dependency('wayland-protocols')
wayland_protocols_dir = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
wayland_scanner = find_program('wayland-scanner')
math_dep = cc.find_library('m', required: true)
rt_dep = cc.find_library('rt', required: false)

language_test = executable(
  'test-language',
  [
    'test-language.c',
    '../src/kasasa-language.c',
  ],
  include_directories: include_directories('../src'),
  dependencies: gio_dep,
)

test(
  'Language selection',
  language_test,
  env: [
    'G_DEBUG=fatal-warnings',
    'GSETTINGS_BACKEND=memory',
    'GSETTINGS_SCHEMA_DIR=' + (meson.project_build_root() / 'data'),
  ],
  suite: 'unit',
  timeout: 30,
)

toplevel_export_xml = files('../protocols/hyprland-toplevel-export-v1.xml')
toplevel_export_client_h = custom_target(
  'test-hyprland-toplevel-export-v1-client.h',
  input: toplevel_export_xml,
  output: 'hyprland-toplevel-export-v1-client.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)
toplevel_export_protocol_c = custom_target(
  'test-hyprland-toplevel-export-v1-protocol.c',
  input: toplevel_export_xml,
  output: 'hyprland-toplevel-export-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)
hyprland_protocol_gen = [toplevel_export_client_h, toplevel_export_protocol_c]

foreach protocol : [
  ['linux-dmabuf-v1', wayland_protocols_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml'],
  ['ext-foreign-toplevel-list-v1', wayland_protocols_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml'],
  ['ext-image-capture-source-v1', wayland_protocols_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml'],
  ['ext-image-copy-capture-v1', wayland_protocols_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml'],
]
  protocol_name = protocol[0]
  protocol_xml = files(protocol[1])
  protocol_client_h = custom_target(
    'test-' + protocol_name + '-client.h',
    input: protocol_xml,
    output: protocol_name + '-client.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_c = custom_target(
    'test-' + protocol_name + '-protocol.c',
    input: protocol_xml,
    output: protocol_name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
  hyprland_protocol_gen += [protocol_client_h, protocol_c]
endforeach

zoom_test = executable(
  'test-zoom',
  [
    'test-zoom.c',
    '../src/kasasa-zoom.c',
  ],
  include_directories: include_directories('../src'),
  dependencies: [glib_dep, math_dep],
)

test(
  'Zoom calculation',
  zoom_test,
  env: ['G_DEBUG=fatal-warnings'],
  suite: 'unit',
  timeout: 30,
)

window_layout_test = executable(
  'test-window-layout',
  [
    'test-window-layout.c',
    '../src/kasasa-window-layout.c',
    '../src/kasasa-zoom.c',
  ],
  include_directories: include_directories('../src'),
  dependencies: [glib_dep, math_dep],
)

test(
  'Window layout calculation',
  window_layout_test,
  env: ['G_DEBUG=fatal-warnings'],
  suite: 'unit',
  timeout: 30,
)

image_test = executable(
  'test-image',
  [
    'test-image.c',
    '../src/kasasa-image.c',
  ],
  include_directories: include_directories('../src'),
  dependencies: gtk_dep,
)

test(
  'Image loading',
  image_test,
  env: ['G_DEBUG=fatal-warnings'],
  suite: 'unit',
  timeout: 30,
)

source_test = executable(
  'test-source',
  [
    'test-source.c',
    '../src/kasasa-source.c',
  ],
  include_directories: include_directories('../src'),
  dependencies: glib_dep,
)

region_capture_test = executable(
  'test-region-capture',
  [
    'test-region-capture.c',
    '../src/kasasa-region-capture.c',
  ],
  c_args: ['-DKASASA_ENABLE_TESTS'],
  include_directories: include_directories('../src'),
  dependencies: [gio_dep, dependency('gdk-pixbuf-2.0')],
)

test(
  'Region capture',
  region_capture_test,
  env: ['G_DEBUG=fatal-warnings'],
  suite: 'unit',
  timeout: 10,
)

test(
  'Source lifecycle',
  source_test,
  env: ['G_DEBUG=fatal-warnings'],
  suite: 'unit',
  timeout: 30,
)

window_query_test = executable(
  'test-window-query',
  [
    'test-window-query.c',
    '../src/kasasa-window-query.c',
    '../src/kasasa-hyprland-stream.c',
    '../src/kasasa-wayland-stubs.c',
    hyprland_protocol_gen,
  ],
  include_directories: include_directories('../src', '.'),
  dependencies: [
    glib_dep,
    json_glib_dep,
    wayland_client_dep,
    gbm_dep,
    libdrm_dep,
    rt_dep,
  ],
)

test(
  'Window query matching',
  window_query_test,
  env: ['G_DEBUG=fatal-warnings'],
  suite: 'unit',
  timeout: 30,
)

hyprland_capture_test = executable(
  'test-hyprland-capture',
  [
    'test-hyprland-capture.c',
    '../src/kasasa-hyprland-capture.c',
    '../src/kasasa-hyprland-stream.c',
    '../src/kasasa-window-query.c',
    '../src/kasasa-wayland-stubs.c',
    hyprland_protocol_gen,
  ],
  c_args: ['-DKASASA_ENABLE_TESTS'],
  include_directories: include_directories('../src', '.'),
  dependencies: [
    gtk_dep,
    json_glib_dep,
    wayland_client_dep,
    gbm_dep,
    libdrm_dep,
    rt_dep,
  ],
)

test(
  'Hyprland capture cancellation',
  hyprland_capture_test,
  env: ['G_DEBUG=fatal-warnings'],
  suite: 'unit',
  timeout: 10,
)

gtk_screenshot_test = executable(
  'test-gtk-screenshot',
  [
    'test-gtk-screenshot.c',
    '../src/kasasa-content.c',
    '../src/kasasa-image.c',
    '../src/kasasa-screenshot.c',
    '../src/kasasa-screencast.c',
    '../src/kasasa-crop-paintable.c',
    '../src/kasasa-dmabuf-paintable.c',
    '../src/kasasa-source.c',
    '../src/kasasa-window-query.c',
    '../src/kasasa-hyprland-stream.c',
    '../src/kasasa-wayland-stubs.c',
    hyprland_protocol_gen,
  ],
  c_args: ['-DKASASA_ENABLE_TESTS'],
  include_directories: include_directories('../src', '.'),
  dependencies: [
    adwaita_dep,
    gstreamer_dep,
    gstreamer_app_dep,
    json_glib_dep,
    wayland_client_dep,
    gbm_dep,
    libdrm_dep,
    rt_dep,
  ],
)

test(
  'Screenshot widget',
  gtk_screenshot_test,
  env: [
    'G_DEBUG=fatal-warnings',
    'GSETTINGS_BACKEND=memory',
    'XDG_CONFIG_HOME=' + (meson.current_build_dir() / 'config'),
    'XDG_DATA_HOME=' + (meson.current_build_dir() / 'data'),
  ],
  suite: 'gtk',
  timeout: 30,
)

gtk_application_test = executable(
  'test-gtk-application',
  [
    'test-gtk-application.c',
    '../src/kasasa-application.c',
    '../src/kasasa-window-query.c',
    '../src/kasasa-hyprland-capture.c',
    '../src/kasasa-hyprland-stream.c',
    '../src/kasasa-wayland-stubs.c',
    hyprland_protocol_gen,
  ],
  include_directories: include_directories('../src', '.'),
  dependencies: [
    adwaita_dep,
    json_glib_dep,
    wayland_client_dep,
    gbm_dep,
    libdrm_dep,
    rt_dep,
  ],
)

test(
  'Application lifecycle',
  gtk_application_test,
  env: [
    'G_DEBUG=fatal-warnings',
    'GSETTINGS_BACKEND=memory',
    'XDG_CONFIG_HOME=' + (meson.current_build_dir() / 'config'),
    'XDG_DATA_HOME=' + (meson.current_build_dir() / 'data'),
  ],
  suite: 'gtk',
  timeout: 30,
)

test_app_resources = gnome.compile_resources(
  'test-preferences-resources',
  '../src/kasasa.gresource.xml',
  source_dir: '../src',
  c_name: 'kasasa_test_preferences',
)

gtk_preferences_test = executable(
  'test-gtk-preferences',
  [
    'test-gtk-preferences.c',
    '../src/kasasa-language.c',
    '../src/kasasa-preferences.c',
    test_app_resources,
  ],
  include_directories: include_directories('../src'),
  dependencies: adwaita_dep,
)

test(
  'Preferences behavior',
  gtk_preferences_test,
  env: [
    'G_DEBUG=fatal-warnings',
    'GSETTINGS_BACKEND=memory',
    'GSETTINGS_SCHEMA_DIR=' + (meson.project_build_root() / 'data'),
    'XDG_CONFIG_HOME=' + (meson.current_build_dir() / 'config'),
    'XDG_DATA_HOME=' + (meson.current_build_dir() / 'data'),
  ],
  suite: 'gtk',
  timeout: 30,
)

gtk_content_container_test = executable(
  'test-gtk-content-container',
  [
    'test-gtk-content-container.c',
    '../src/kasasa-content-container.c',
    '../src/kasasa-region-capture.c',
    '../src/kasasa-content.c',
    '../src/kasasa-image.c',
    '../src/kasasa-screenshot.c',
    '../src/kasasa-screencast.c',
    '../src/kasasa-crop-paintable.c',
    '../src/kasasa-dmabuf-paintable.c',
    '../src/kasasa-source.c',
    '../src/kasasa-window-query.c',
    '../src/kasasa-window-picker.c',
    '../src/kasasa-hyprland-capture.c',
    '../src/kasasa-hyprland-stream.c',
    '../src/kasasa-wayland-stubs.c',
    hyprland_protocol_gen,
    test_app_resources,
  ],
  include_directories: include_directories('../src', '.'),
  dependencies: [
    adwaita_dep,
    gstreamer_dep,
    gstreamer_app_dep,
    math_dep,
    json_glib_dep,
    wayland_client_dep,
    gbm_dep,
    libdrm_dep,
    rt_dep,
  ],
)

test(
  'Content container behavior',
  gtk_content_container_test,
  env: [
    'G_DEBUG=fatal-warnings',
    'GSETTINGS_BACKEND=memory',
    'GSETTINGS_SCHEMA_DIR=' + (meson.project_build_root() / 'data'),
    'XDG_CONFIG_HOME=' + (meson.current_build_dir() / 'config'),
    'XDG_DATA_HOME=' + (meson.current_build_dir() / 'data'),
  ],
  suite: 'gtk',
  timeout: 30,
)

gtk_window_test = executable(
  'test-gtk-window',
  [
    'test-gtk-window.c',
    '../src/kasasa-application.c',
    '../src/kasasa-language.c',
    '../src/kasasa-window.c',
    '../src/kasasa-window-layout.c',
    '../src/kasasa-preferences.c',
    '../src/kasasa-content-container.c',
    '../src/kasasa-region-capture.c',
    '../src/kasasa-content.c',
    '../src/kasasa-image.c',
    '../src/kasasa-screenshot.c',
    '../src/kasasa-screencast.c',
    '../src/kasasa-crop-paintable.c',
    '../src/kasasa-dmabuf-paintable.c',
    '../src/kasasa-source.c',
    '../src/kasasa-zoom.c',
    '../src/kasasa-window-query.c',
    '../src/kasasa-window-picker.c',
    '../src/kasasa-hyprland-capture.c',
    '../src/kasasa-hyprland-stream.c',
    '../src/kasasa-wayland-stubs.c',
    hyprland_protocol_gen,
    test_app_resources,
  ],
  include_directories: include_directories('../src', '.'),
  dependencies: [
    adwaita_dep,
    gstreamer_dep,
    gstreamer_app_dep,
    math_dep,
    json_glib_dep,
    wayland_client_dep,
    gbm_dep,
    libdrm_dep,
    rt_dep,
  ],
)

test(
  'Window cleanup behavior',
  gtk_window_test,
  env: [
    'G_DEBUG=fatal-warnings',
    'GSETTINGS_BACKEND=memory',
    'GSETTINGS_SCHEMA_DIR=' + (meson.project_build_root() / 'data'),
    'XDG_CONFIG_HOME=' + (meson.current_build_dir() / 'config'),
    'XDG_DATA_HOME=' + (meson.current_build_dir() / 'data'),
  ],
  suite: 'gtk',
  timeout: 30,
)

if get_option('integration_tests')
  wayland_integration_deps = [gtk_dep]
  wayland_integration_c_args = []
  if gtk_layer_shell_dep.found()
    wayland_integration_deps = [
      gtk_layer_shell_dep,
      gtk_dep,
      wayland_client_dep,
    ]
    wayland_integration_c_args += '-DKASASA_HAVE_LAYER_SHELL'
  endif

  wayland_integration_test = executable(
    'test-wayland-integration',
    'test-wayland-integration.c',
    dependencies: wayland_integration_deps,
    c_args: wayland_integration_c_args,
  )

  test(
    'Wayland/Hyprland session',
    wayland_integration_test,
    env: ['G_DEBUG=fatal-warnings'],
    suite: 'integration',
    timeout: 30,
    is_parallel: false,
  )
endif

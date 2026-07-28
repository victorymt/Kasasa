glib_dep = dependency('glib-2.0')
gio_dep = dependency('gio-2.0')
gtk_dep = dependency('gtk4')
adwaita_dep = dependency('libadwaita-1', version: '>= 1.7.4')
portal_dep = dependency('libportal')
portal_gtk_dep = dependency('libportal-gtk4')
gstreamer_dep = dependency('gstreamer-1.0')
gstreamer_app_dep = dependency('gstreamer-app-1.0')
json_glib_dep = dependency('json-glib-1.0')
wayland_client_dep = dependency('wayland-client')
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
  dependencies: [glib_dep, json_glib_dep, wayland_client_dep, rt_dep],
)

test(
  'Window query matching',
  window_query_test,
  env: ['G_DEBUG=fatal-warnings'],
  suite: 'unit',
  timeout: 30,
)

gtk_screenshot_test = executable(
  'test-gtk-screenshot',
  [
    'test-gtk-screenshot.c',
    '../src/kasasa-content.c',
    '../src/kasasa-image.c',
    '../src/kasasa-screenshot.c',
    '../src/kasasa-screencast.c',
    '../src/kasasa-screencast-pipeline.c',
    '../src/kasasa-source.c',
    '../src/kasasa-window-query.c',
    '../src/kasasa-hyprland-stream.c',
    '../src/kasasa-wayland-stubs.c',
    hyprland_protocol_gen,
  ],
  include_directories: include_directories('../src', '.'),
  dependencies: [
    adwaita_dep,
    portal_dep,
    gstreamer_dep,
    gstreamer_app_dep,
    json_glib_dep,
    wayland_client_dep,
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
  dependencies: [adwaita_dep, json_glib_dep, wayland_client_dep, rt_dep],
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
    '../src/kasasa-content.c',
    '../src/kasasa-image.c',
    '../src/kasasa-screenshot.c',
    '../src/kasasa-screencast.c',
    '../src/kasasa-screencast-pipeline.c',
    '../src/kasasa-source.c',
    '../src/kasasa-window-query.c',
    '../src/kasasa-hyprland-stream.c',
    '../src/kasasa-wayland-stubs.c',
    hyprland_protocol_gen,
    test_app_resources,
  ],
  include_directories: include_directories('../src', '.'),
  dependencies: [
    adwaita_dep,
    portal_dep,
    portal_gtk_dep,
    gstreamer_dep,
    gstreamer_app_dep,
    math_dep,
    json_glib_dep,
    wayland_client_dep,
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
    '../src/kasasa-preferences.c',
    '../src/kasasa-content-container.c',
    '../src/kasasa-content.c',
    '../src/kasasa-image.c',
    '../src/kasasa-screenshot.c',
    '../src/kasasa-screencast.c',
    '../src/kasasa-screencast-pipeline.c',
    '../src/kasasa-source.c',
    '../src/kasasa-zoom.c',
    '../src/kasasa-window-query.c',
    '../src/kasasa-hyprland-capture.c',
    '../src/kasasa-hyprland-stream.c',
    '../src/kasasa-wayland-stubs.c',
    hyprland_protocol_gen,
    test_app_resources,
  ],
  include_directories: include_directories('../src', '.'),
  dependencies: [
    adwaita_dep,
    portal_dep,
    portal_gtk_dep,
    gstreamer_dep,
    gstreamer_app_dep,
    math_dep,
    json_glib_dep,
    wayland_client_dep,
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
  wayland_integration_test = executable(
    'test-wayland-integration',
    'test-wayland-integration.c',
    dependencies: gtk_dep,
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

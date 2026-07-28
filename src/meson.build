wayland_scanner = find_program('wayland-scanner')
wayland_client_dep = dependency('wayland-client')
wayland_protocols_gen = []

toplevel_export_xml = files('../protocols/hyprland-toplevel-export-v1.xml')

toplevel_export_client_h = custom_target(
  'hyprland-toplevel-export-v1-client.h',
  input: toplevel_export_xml,
  output: 'hyprland-toplevel-export-v1-client.h',
  command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
)

toplevel_export_protocol_c = custom_target(
  'hyprland-toplevel-export-v1-protocol.c',
  input: toplevel_export_xml,
  output: 'hyprland-toplevel-export-v1-protocol.c',
  command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_protocols_gen += [toplevel_export_client_h, toplevel_export_protocol_c]

kasasa_sources = [
  'main.c',
  'kasasa-application.c',
  'kasasa-window.c',
  'kasasa-preferences.c',
  'kasasa-content.c',
  'kasasa-image.c',
  'kasasa-source.c',
  'kasasa-zoom.c',
  'kasasa-screenshot.c',
  'kasasa-screencast.c',
  'kasasa-screencast-pipeline.c',
  'kasasa-content-container.c',
  'kasasa-window-query.c',
  'kasasa-hyprland-capture.c',
  'kasasa-hyprland-stream.c',
  'kasasa-wayland-stubs.c',
  wayland_protocols_gen,
]

kasasa_deps = [
  dependency('gtk4'),
  dependency('libadwaita-1', version: '>= 1.7.4'),
  dependency('libportal'),
  dependency('libportal-gtk4'),
  dependency('gstreamer-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('json-glib-1.0'),
  wayland_client_dep,
]

kasasa_deps += cc.find_library('m', required : true)
kasasa_deps += cc.find_library('rt', required : false)

kasasa_sources += gnome.compile_resources('kasasa-resources',
  'kasasa.gresource.xml',
  c_name: 'kasasa'
)

executable('kasasa', kasasa_sources,
  dependencies: kasasa_deps,
  include_directories: include_directories('.'),
       install: true,
)

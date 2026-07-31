wayland_scanner = find_program('wayland-scanner')
wayland_client_dep = dependency('wayland-client')
wayland_protocols_dep = dependency('wayland-protocols')
wayland_protocols_dir = wayland_protocols_dep.get_variable(pkgconfig: 'pkgdatadir')
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

foreach protocol : [
  ['linux-dmabuf-v1', wayland_protocols_dir / 'stable/linux-dmabuf/linux-dmabuf-v1.xml'],
  ['ext-foreign-toplevel-list-v1', wayland_protocols_dir / 'staging/ext-foreign-toplevel-list/ext-foreign-toplevel-list-v1.xml'],
  ['ext-image-capture-source-v1', wayland_protocols_dir / 'staging/ext-image-capture-source/ext-image-capture-source-v1.xml'],
  ['ext-image-copy-capture-v1', wayland_protocols_dir / 'staging/ext-image-copy-capture/ext-image-copy-capture-v1.xml'],
]
  protocol_name = protocol[0]
  protocol_xml = files(protocol[1])

  protocol_client_h = custom_target(
    protocol_name + '-client.h',
    input: protocol_xml,
    output: protocol_name + '-client.h',
    command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  protocol_c = custom_target(
    protocol_name + '-protocol.c',
    input: protocol_xml,
    output: protocol_name + '-protocol.c',
    command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
  )
  wayland_protocols_gen += [protocol_client_h, protocol_c]
endforeach

kasasa_sources = [
  'main.c',
  'kasasa-application.c',
  'kasasa-language.c',
  'kasasa-window.c',
  'kasasa-preferences.c',
  'kasasa-content.c',
  'kasasa-image.c',
  'kasasa-source.c',
  'kasasa-zoom.c',
  'kasasa-screenshot.c',
  'kasasa-screencast.c',
  'kasasa-dmabuf-paintable.c',
  'kasasa-screencast-pipeline.c',
  'kasasa-content-container.c',
  'kasasa-window-query.c',
  'kasasa-window-picker.c',
  'kasasa-hyprland-capture.c',
  'kasasa-hyprland-stream.c',
  'kasasa-wayland-stubs.c',
  wayland_protocols_gen,
]

kasasa_deps = [
  dependency('gtk4', version: '>= 4.14'),
  dependency('libadwaita-1', version: '>= 1.7.4'),
  dependency('libportal'),
  dependency('libportal-gtk4'),
  dependency('gstreamer-1.0'),
  dependency('gstreamer-app-1.0'),
  dependency('json-glib-1.0'),
  dependency('gbm'),
  dependency('libdrm'),
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

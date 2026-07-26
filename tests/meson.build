glib_dep = dependency('glib-2.0')
gtk_dep = dependency('gtk4')
adwaita_dep = dependency('libadwaita-1', version: '>= 1.7.4')
math_dep = cc.find_library('m', required: true)

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

crop_test = executable(
  'test-crop',
  [
    'test-crop.c',
    '../src/kasasa-crop.c',
  ],
  include_directories: include_directories('../src'),
  dependencies: glib_dep,
)

test(
  'Crop calculation',
  crop_test,
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

gtk_screenshot_test = executable(
  'test-gtk-screenshot',
  [
    'test-gtk-screenshot.c',
    '../src/kasasa-content.c',
    '../src/kasasa-image.c',
    '../src/kasasa-screenshot.c',
  ],
  include_directories: include_directories('../src'),
  dependencies: adwaita_dep,
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

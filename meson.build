project('flutter-subsurface-prototype', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=c11'])

gtk3_dep = dependency('gtk+-3.0')
wayland_client_dep = dependency('wayland-client')

executable('flutter-subsurface-prototype',
  sources: [
    'src/main.c',
    'src/subsurface_widget.c',
  ],
  dependencies: [gtk3_dep, wayland_client_dep],
  install: false)

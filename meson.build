project('vkwc', 'c')

add_project_arguments([
  '-DWLR_USE_UNSTABLE',
], language: 'c')

wayland_protos = dependency('wayland-protocols', version: '>=1.13')
wl_protocol_dir = wayland_protos.get_pkgconfig_variable('pkgdatadir')
wayland_scanner = find_program('wayland-scanner')
wayland_server = dependency('wayland-server')
# For -lm
cc = meson.get_compiler('c')
math = cc.find_library('m', required: true)

wayland_scanner_code = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.c',
  arguments: ['private-code', '@INPUT@', '@OUTPUT@'],
)

wayland_scanner_server = generator(
  wayland_scanner,
  output: '@BASENAME@-protocol.h',
  arguments: ['server-header', '@INPUT@', '@OUTPUT@'],
)

client_protocols = [
  [wl_protocol_dir + '/stable/xdg-shell', 'xdg-shell.xml'],
]

foreach p : client_protocols
  xml = join_paths(p)
  src = wayland_scanner_code.process(xml)
  header = wayland_scanner_server.process(xml)

  name = p[1].split('.')[0].underscorify()

  lib = static_library(
    name,
    [src, header],
    dependencies: [wayland_server],
  )

  dep = declare_dependency(
    link_with: lib,
    sources: header,
  )

  set_variable(name, dep)
endforeach

wlroots = dependency('wlroots')
vulkan = dependency('vulkan')
pixman = dependency('pixman-1')
xkbcommon = dependency('xkbcommon')
cglm = dependency('cglm')

sources = files(
  'vkwc.c',
  'vulkan/pixel_format.c',
  'vulkan/renderer.c',
  'vulkan/texture.c',
  'vulkan/error.c',
  'vulkan/util.c',
  'vulkan/vulkan.c',
  'vulkan/render_pass.c',
  'vulkan/pipeline.c',
  'render.c',
  'util.c',
  'surface.c',
  'misc/pixel_format.c',
)

subdir('vulkan/shaders')

executable(
	'vkwc',
	sources,
	dependencies: [
      math,
      wlroots,
      vulkan,
      pixman,
      xkbcommon,
      xdg_shell,
      cglm,
    ]
)


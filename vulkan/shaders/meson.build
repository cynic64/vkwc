vulkan_shaders_src = [
  'quad.frag',
  'common.vert',
  'texture.frag',
  'simple_texture.frag',
  'postprocess.vert',
  'postprocess.frag',
  'blur.frag',
]

glslang = find_program('glslangValidator', native: true, required: true)

vulkan_shaders = []

foreach shader : vulkan_shaders_src
  name = shader.underscorify() + '_data'
  args = [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', name]
  header = custom_target(
    shader + '_spv',
    output: shader + '.h',
    input: shader,
    command: args
  )

  vulkan_shaders += [header]
endforeach

sources += vulkan_shaders

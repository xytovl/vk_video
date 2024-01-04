project('vk_video', 'cpp',
  version : '0.1',
  default_options : ['warning_level=3',
                     'cpp_std=c++20'])

add_global_arguments('-DVULKAN_HPP_NO_CONSTRUCTORS', language: 'cpp')

vk = dependency('vulkan')

glsllang = find_program('glslangValidator')


y = custom_target('pattern_y',
  output: 'pattern_y.h',
  input: 'pattern.comp',
  command: [glsllang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', 'spirv_pattern_y', '-DY']
  )
cb_cr = custom_target('pattern_cb_cr',
  output: 'pattern_cb_cr.h',
  input: 'pattern.comp',
  command: [glsllang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', 'spirv_pattern_cb_cr']
  )

exe = executable('vk_video', ['vk_video.cpp', y, cb_cr],
  dependencies: [vk],
  install : true)

test('basic', exe)

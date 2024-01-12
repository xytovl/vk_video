project('vk_video', 'cpp',
  version : '0.1',
  default_options : ['warning_level=3',
                     'cpp_std=c++20'])

add_global_arguments('-DVULKAN_HPP_NO_CONSTRUCTORS', language: 'cpp')
add_global_arguments('-DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1', language: 'cpp')

vk = dependency('vulkan')

glsllang = find_program('glslangValidator')


pattern = custom_target('pattern',
  output: 'spirv_pattern.h',
  input: 'pattern.comp',
  command: [glsllang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', 'spirv_pattern']
  )

exe = executable('vk_video',
  ['vk_video.cpp',
   'video_encoder.cpp',
   'video_encoder_h264.cpp',
   'slot_info.cpp',
   'test_pattern.cpp',
   'memory_allocator.cpp',
   pattern],
  dependencies: [vk],
  install : true)

test('basic', exe)

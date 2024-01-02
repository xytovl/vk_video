project('vk_video', 'cpp',
  version : '0.1',
  default_options : ['warning_level=3',
                     'cpp_std=c++20'])

add_global_arguments('-DVULKAN_HPP_NO_CONSTRUCTORS', language: 'cpp')

vk = dependency('vulkan')

exe = executable('vk_video', 'vk_video.cpp',
  dependencies: [vk],
  install : true)

test('basic', exe)

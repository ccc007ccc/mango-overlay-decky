wire_inc = include_directories('include')

wire_lib = static_library(
  'mango-overlay-wire-scene',
  'src/scene_codec.cpp',
  include_directories: wire_inc,
  dependencies: [protocol_dep, scene_dep],
  install: false,
)

wire_dep = declare_dependency(
  include_directories: wire_inc,
  link_with: wire_lib,
  dependencies: [protocol_dep, scene_dep],
)

if get_option('tests').enabled()
  wire_scene_codec_test = executable(
    'mango-overlay-wire-scene-codec-test',
    'tests/scene_codec_test.cpp',
    dependencies: wire_dep,
  )
  test('mango overlay wire scene codec', wire_scene_codec_test)
endif

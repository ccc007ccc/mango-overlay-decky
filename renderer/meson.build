renderer_inc = include_directories('include')

renderer_lib = static_library(
  'mango-overlay-renderer-core',
  'src/canvas.cpp',
  'src/layout.cpp',
  'src/scene_client.cpp',
  'src/scene_mirror.cpp',
  'src/texture_cache.cpp',
  include_directories: renderer_inc,
  dependencies: [protocol_dep, scene_dep, wire_dep, dep_pthread],
  install: false,
)

renderer_dep = declare_dependency(
  include_directories: renderer_inc,
  link_with: renderer_lib,
  dependencies: [protocol_dep, scene_dep, wire_dep, dep_pthread],
)

renderer_imgui_lib = static_library(
  'mango-overlay-renderer-imgui',
  'src/imgui_scene.cpp',
  include_directories: renderer_inc,
  dependencies: [renderer_dep, dearimgui_dep],
  install: false,
)

renderer_imgui_dep = declare_dependency(
  include_directories: renderer_inc,
  link_with: renderer_imgui_lib,
  dependencies: [renderer_dep, dearimgui_dep],
)

renderer_opengl_texture_lib = static_library(
  'mango-overlay-renderer-opengl-texture',
  'src/opengl_texture_backend.cpp',
  include_directories: renderer_inc,
  dependencies: [renderer_dep, dependency('gl')],
  install: false,
)

renderer_opengl_texture_dep = declare_dependency(
  include_directories: renderer_inc,
  link_with: renderer_opengl_texture_lib,
  dependencies: [renderer_dep, dependency('gl')],
)

if get_option('tests').enabled()
  renderer_scene_mirror_test = executable(
    'mango-overlay-renderer-scene-mirror-test',
    'tests/scene_mirror_test.cpp',
    dependencies: [renderer_dep, broker_dep],
  )
  test('mango overlay renderer scene mirror', renderer_scene_mirror_test)

  renderer_canvas_test = executable(
    'mango-overlay-renderer-canvas-test',
    'tests/canvas_test.cpp',
    dependencies: renderer_dep,
  )
  test('mango overlay renderer canvas transform', renderer_canvas_test)

  renderer_layout_test = executable(
    'mango-overlay-renderer-layout-test',
    'tests/layout_test.cpp',
    dependencies: renderer_dep,
  )
  test('mango overlay renderer scene layout', renderer_layout_test)

  renderer_scene_client_test = executable(
    'mango-overlay-renderer-scene-client-test',
    'tests/scene_client_process_test.cpp',
    dependencies: [renderer_dep, protocol_dep],
  )
  test(
    'mango overlay renderer scene client',
    renderer_scene_client_test,
    args: [mango_overlayd.full_path()],
    depends: mango_overlayd,
  )

  renderer_resource_mirror_test = executable(
    'mango-overlay-renderer-resource-mirror-test',
    'tests/resource_mirror_test.cpp',
    dependencies: [renderer_dep, broker_dep],
  )
  test('mango overlay renderer resource mirror', renderer_resource_mirror_test)

  renderer_texture_cache_test = executable(
    'mango-overlay-renderer-texture-cache-test',
    'tests/texture_cache_test.cpp',
    dependencies: renderer_dep,
  )
  test('mango overlay renderer texture cache', renderer_texture_cache_test)

  renderer_imgui_scene_test = executable(
    'mango-overlay-renderer-imgui-scene-test',
    'tests/imgui_scene_test.cpp',
    dependencies: renderer_imgui_dep,
  )
  test('mango overlay renderer ImGui scene', renderer_imgui_scene_test)
endif

resource_inc = include_directories('include')

resource_png_dep = dependency('libpng')
resource_jpeg_dep = dependency('libjpeg')
resource_webp_dep = dependency('libwebp')
resource_gif_dep = cpp.find_library('gif')

resource_lib = static_library(
  'mango-overlay-resource',
  'src/image.cpp',
  include_directories: resource_inc,
  dependencies: [
    resource_png_dep,
    resource_jpeg_dep,
    resource_webp_dep,
    resource_gif_dep,
  ],
  install: false,
)

resource_dep = declare_dependency(
  include_directories: resource_inc,
  link_with: resource_lib,
  dependencies: [
    resource_png_dep,
    resource_jpeg_dep,
    resource_webp_dep,
    resource_gif_dep,
  ],
)

if get_option('tests').enabled()
  resource_image_test = executable(
    'mango-overlay-resource-image-test',
    'tests/image_test.cpp',
    dependencies: resource_dep,
  )
  test('mango overlay image decoding', resource_image_test)
endif

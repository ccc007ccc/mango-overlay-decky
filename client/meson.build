client_inc = include_directories('include')

client_lib = shared_library(
  'mango-overlay-client',
  'src/client.cpp',
  include_directories: client_inc,
  dependencies: [protocol_dep, scene_dep, wire_dep],
  version: '1.0.0',
  soversion: '1',
  install: true,
)

client_dep = declare_dependency(
  include_directories: client_inc,
  link_with: client_lib,
  dependencies: [protocol_dep, scene_dep, wire_dep],
)

install_headers(
  'include/mango_overlay/client.h',
  'include/mango_overlay/client.hpp',
  subdir: 'mango_overlay',
)

prog_python.install_sources(
  'python/mango_overlay/__init__.py',
  subdir: 'mango_overlay',
)

if get_option('tests').enabled()
  client_process_test = executable(
    'mango-overlay-provider-client-process-test',
    'tests/provider_client_process_test.cpp',
    dependencies: client_dep,
  )
  test(
    'mango overlay provider client process',
    client_process_test,
    args: [mango_overlayd.full_path()],
    depends: mango_overlayd,
  )

  client_c_header_test = executable(
    'mango-overlay-client-c-header-test',
    'tests/c_api_header_test.c',
    dependencies: client_dep,
  )
  test('mango overlay client C header', client_c_header_test)

  client_cpp_process_test = executable(
    'mango-overlay-cpp-client-process-test',
    'tests/cpp_client_process_test.cpp',
    dependencies: client_dep,
  )
  test(
    'mango overlay C++ provider client',
    prog_python,
    args: [
      files('tests/cpp_client_process_test.py'),
      mango_overlayd.full_path(),
      client_cpp_process_test.full_path(),
      client_lib.full_path(),
    ],
    depends: [mango_overlayd, client_cpp_process_test, client_lib],
  )

  test(
    'mango overlay Python provider client',
    prog_python,
    args: [
      files('tests/python_client_process_test.py'),
      mango_overlayd.full_path(),
      client_lib.full_path(),
      meson.current_source_dir() / 'python',
    ],
    depends: [mango_overlayd, client_lib],
  )

  cargo = find_program('cargo', required: false)
  if cargo.found()
    test(
      'mango overlay Rust provider client',
      prog_python,
      args: [
        files('tests/rust_client_process_test.py'),
        cargo.full_path(),
        mango_overlayd.full_path(),
        client_lib.full_path(),
        meson.current_source_dir() / 'rust/Cargo.toml',
        meson.current_build_dir() / 'rust-target',
      ],
      depends: [mango_overlayd, client_lib],
      timeout: 120,
    )
  endif
endif

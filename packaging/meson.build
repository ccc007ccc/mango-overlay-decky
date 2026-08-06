if get_option('tests').enabled()
  test(
    'mango overlay Decky package',
    prog_python,
    args: [
      '-m',
      'unittest',
      'discover',
      '-s',
      meson.current_source_dir() / 'tests',
      '-p',
      'test_*.py',
    ],
    env: {
      'PYTHONPATH': meson.current_source_dir(),
    },
  )
endif

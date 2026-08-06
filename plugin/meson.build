test(
  'mango overlay lifecycle',
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
    'PYTHONPATH': meson.current_source_dir() / 'py_modules',
  },
)

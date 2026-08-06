scene_inc = include_directories('include')

scene_lib = static_library(
  'mango-overlay-scene',
  'src/store.cpp',
  'src/validation.cpp',
  include_directories: scene_inc,
  dependencies: resource_dep,
  install: false,
)

scene_dep = declare_dependency(
  include_directories: scene_inc,
  link_with: scene_lib,
  dependencies: resource_dep,
)

if get_option('tests').enabled()
  scene_transaction_test = executable(
    'mango-overlay-scene-transaction-test',
    'tests/transaction_test.cpp',
    dependencies: scene_dep,
  )
  test('mango overlay atomic scene transaction', scene_transaction_test)

  scene_change_feed_test = executable(
    'mango-overlay-scene-change-feed-test',
    'tests/change_feed_test.cpp',
    dependencies: scene_dep,
  )
  test('mango overlay scene change feed', scene_change_feed_test)

  scene_size_limit_test = executable(
    'mango-overlay-scene-size-limit-test',
    'tests/size_limit_test.cpp',
    dependencies: scene_dep,
  )
  test('mango overlay scene size limit', scene_size_limit_test)

  scene_primitives_test = executable(
    'mango-overlay-scene-primitives-test',
    'tests/primitives_test.cpp',
    dependencies: scene_dep,
  )
  test('mango overlay scene primitives', scene_primitives_test)

  scene_hierarchy_test = executable(
    'mango-overlay-scene-hierarchy-test',
    'tests/hierarchy_test.cpp',
    dependencies: scene_dep,
  )
  test('mango overlay scene hierarchy', scene_hierarchy_test)

  scene_resource_lifecycle_test = executable(
    'mango-overlay-scene-resource-lifecycle-test',
    'tests/resource_lifecycle_test.cpp',
    dependencies: scene_dep,
  )
  test('mango overlay scene resource lifecycle', scene_resource_lifecycle_test)

  scene_policy_test = executable(
    'mango-overlay-scene-policy-test',
    'tests/policy_test.cpp',
    dependencies: scene_dep,
  )
  test('mango overlay scene policy', scene_policy_test)
endif

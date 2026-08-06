broker_inc = include_directories('include')

broker_lib = static_library(
  'mango-overlay-broker',
  'src/handshake_processor.cpp',
  'src/connection.cpp',
  'src/provider_session.cpp',
  'src/renderer_session.cpp',
  'src/scene_decoder.cpp',
  'src/snapshot_encoder.cpp',
  'src/policy_file.cpp',
  'src/controller_session.cpp',
  include_directories: broker_inc,
  dependencies: [protocol_dep, scene_dep, wire_dep],
  install: false,
)

broker_dep = declare_dependency(
  include_directories: broker_inc,
  link_with: broker_lib,
  dependencies: [protocol_dep, scene_dep, wire_dep],
)

mango_overlayd = executable(
  'mango-overlayd',
  'src/main.cpp',
  dependencies: [broker_dep, dep_pthread],
  install: true,
)

mango_overlayctl = executable(
  'mango-overlayctl',
  'src/control_main.cpp',
  dependencies: protocol_dep,
  install: true,
)

if get_option('tests').enabled()
  test(
    'mango overlay broker self test',
    mango_overlayd,
    args: ['--mango-overlay-self-test'],
    timeout: 5,
  )

  test(
    'mango overlay controller process',
    prog_python,
    args: [
      files('tests/controller_process_test.py'),
      mango_overlayd.full_path(),
      mango_overlayctl.full_path(),
    ],
    depends: [mango_overlayd, mango_overlayctl],
    timeout: 20,
  )

  broker_handshake_test = executable(
    'mango-overlay-broker-handshake-test',
    'tests/handshake_processor_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay broker handshake', broker_handshake_test)

  broker_handshake_rejection_test = executable(
    'mango-overlay-broker-handshake-rejection-test',
    'tests/handshake_rejection_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay broker handshake rejection', broker_handshake_rejection_test)

  broker_connection_test = executable(
    'mango-overlay-broker-connection-test',
    'tests/connection_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay broker seqpacket connection', broker_connection_test)

  broker_socket_policy_test = executable(
    'mango-overlay-broker-socket-policy-test',
    'tests/socket_policy_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay broker socket policy', broker_socket_policy_test)

  broker_process_test = executable(
    'mango-overlay-broker-process-test',
    'tests/broker_process_test.cpp',
    dependencies: protocol_dep,
  )
  test(
    'mango overlay broker process',
    broker_process_test,
    args: [mango_overlayd.full_path()],
    depends: mango_overlayd,
  )

  broker_multi_provider_test = executable(
    'mango-overlay-broker-multi-provider-test',
    'tests/multi_provider_process_test.cpp',
    dependencies: protocol_dep,
  )
  test(
    'mango overlay broker concurrent providers',
    broker_multi_provider_test,
    args: [mango_overlayd.full_path()],
    depends: mango_overlayd,
  )

  broker_renderer_process_test = executable(
    'mango-overlay-broker-renderer-process-test',
    'tests/renderer_process_test.cpp',
    dependencies: protocol_dep,
  )
  test(
    'mango overlay broker renderer stream',
    broker_renderer_process_test,
    args: [mango_overlayd.full_path()],
    depends: mango_overlayd,
  )

  provider_session_test = executable(
    'mango-overlay-provider-session-test',
    'tests/provider_session_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay provider session registration', provider_session_test)

  scene_decode_test = executable(
    'mango-overlay-scene-decode-test',
    'tests/scene_decoder_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay scene transaction decode', scene_decode_test)

  snapshot_encoder_test = executable(
    'mango-overlay-snapshot-encoder-test',
    'tests/snapshot_encoder_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay snapshot encoding', snapshot_encoder_test)

  renderer_session_test = executable(
    'mango-overlay-renderer-session-test',
    'tests/renderer_session_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay renderer session', renderer_session_test)

  provider_transaction_test = executable(
    'mango-overlay-provider-transaction-test',
    'tests/provider_transaction_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay provider transaction', provider_transaction_test)

  provider_error_test = executable(
    'mango-overlay-provider-error-test',
    'tests/provider_error_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay provider error handling', provider_error_test)

  provider_resource_test = executable(
    'mango-overlay-provider-resource-test',
    'tests/provider_resource_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay provider resources', provider_resource_test)

  broker_policy_file_test = executable(
    'mango-overlay-policy-file-test',
    'tests/policy_file_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay policy file', broker_policy_file_test)

  broker_controller_session_test = executable(
    'mango-overlay-controller-session-test',
    'tests/controller_session_test.cpp',
    dependencies: broker_dep,
  )
  test('mango overlay controller session', broker_controller_session_test)
endif

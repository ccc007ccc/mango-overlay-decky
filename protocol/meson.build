protocol_inc = include_directories('include', '.')
flatbuffers_dep = dependency('flatbuffers').partial_dependency(
  compile_args: true,
  includes: true,
)
flatc = find_program('flatc')

protocol_wire_header = custom_target(
  'mango-overlay-wire-header',
  input: 'schema/mango_overlay.fbs',
  output: 'mango_overlay_generated.h',
  command: [flatc, '--cpp', '--scoped-enums', '-o', '@OUTDIR@', '@INPUT@'],
)

protocol_lib = static_library(
  'mango-overlay-protocol',
  'src/packet.cpp',
  'src/handshake.cpp',
  'src/error.cpp',
  'src/registration.cpp',
  'src/renderer.cpp',
  'src/controller.cpp',
  'src/transaction.cpp',
  'src/unix_seqpacket.cpp',
  protocol_wire_header,
  include_directories: protocol_inc,
  dependencies: flatbuffers_dep,
  install: false,
)

protocol_dep = declare_dependency(
  include_directories: protocol_inc,
  link_with: protocol_lib,
  dependencies: flatbuffers_dep,
  sources: protocol_wire_header,
)

if get_option('tests').enabled()
  packet_test = executable(
    'mango-overlay-packet-test',
    'tests/packet_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay packet', packet_test)

  packet_decode_test = executable(
    'mango-overlay-packet-decode-test',
    'tests/packet_decode_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay packet decode', packet_decode_test)

  hello_decode_test = executable(
    'mango-overlay-hello-decode-test',
    'tests/hello_decode_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay hello decode', hello_decode_test)

  hello_encode_test = executable(
    'mango-overlay-hello-encode-test',
    'tests/hello_encode_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay hello encode', hello_encode_test)

  handshake_test = executable(
    'mango-overlay-handshake-test',
    'tests/handshake_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay handshake', handshake_test)

  hello_accepted_encode_test = executable(
    'mango-overlay-hello-accepted-encode-test',
    'tests/hello_accepted_encode_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay hello accepted encode', hello_accepted_encode_test)

  registration_decode_test = executable(
    'mango-overlay-registration-decode-test',
    'tests/registration_decode_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay provider registration decode', registration_decode_test)

  renderer_subscription_test = executable(
    'mango-overlay-renderer-subscription-test',
    'tests/renderer_subscription_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay renderer subscription', renderer_subscription_test)

  controller_protocol_test = executable(
    'mango-overlay-controller-protocol-test',
    'tests/controller_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay controller protocol', controller_protocol_test)

  unix_seqpacket_test = executable(
    'mango-overlay-unix-seqpacket-test',
    'tests/unix_seqpacket_test.cpp',
    dependencies: protocol_dep,
  )
  test('mango overlay Unix seqpacket attachments', unix_seqpacket_test)
endif

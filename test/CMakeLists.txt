list(APPEND tests
  lookup
  lookup-invalid
  lookup-ipv6
  socket-send-recv
  socket-send-recv-dualstack
  socket-send-recv-ipv6
  stream-destroy
  stream-destroy-before-connect
  stream-preconnect
  stream-preconnect-same-socket
  stream-relay
  stream-send-recv
  stream-send-recv-ipv6
  stream-write-read
  stream-write-read-ipv6
)

foreach(test IN LISTS tests)
  add_executable(test_${test} ${test}.c)

  target_link_libraries(test_${test} udx_shared)

  add_test(${test} test_${test})
endforeach()

list(APPEND tests
  lookup
  lookup-invalid
  lookup-ipv6
  socket-send-recv
  socket-send-recv-dualstack
  socket-send-recv-ipv6
  stream-combined-write-read
  stream-destroy
  stream-destroy-before-connect
  stream-preconnect
  stream-preconnect-same-socket
  stream-relay
  stream-send-recv
  stream-send-recv-ipv6
  stream-write-read
  stream-write-read-ipv6
  stream-write-read-perf
)

list(APPEND skipped_tests
  stream-write-read-perf
)

foreach(test IN LISTS tests)
  add_executable(${test} ${test}.c)

  target_link_libraries(
    ${test}
    PRIVATE
      udx_static
  )

  add_test(
    NAME ${test}
    COMMAND ${test}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
  )

  set_tests_properties(
    ${test}
    PROPERTIES
    TIMEOUT 30000
  )

  if(${test} IN_LIST skipped_tests)
    set_tests_properties(
      ${test}
      PROPERTIES
      DISABLED True
    )
  endif()
endforeach()

list(APPEND tests
  socket-send-recv
  stream-preconnect
  stream-preconnect-same-socket
  stream-send-recv
  stream-write-read
)

foreach(test IN LISTS tests)
  add_executable(test_${test} ${test}.c)

  target_link_libraries(test_${test} udx ${UV})

  add_test(${test} test_${test})
endforeach()
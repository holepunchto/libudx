add_executable(server server.c)
add_executable(client client.c)
add_executable(udxperf udxperf.c)
add_executable(sledgetime sledgetime.c)

target_link_libraries(
  server
  PRIVATE
    udx_static
)

target_link_libraries(
  client
  PRIVATE
    udx_static
)

target_link_libraries(
  udxperf
  PRIVATE
    udx_static
)

target_link_libraries(
  sledgetime
  PRIVATE
    udx_static
)


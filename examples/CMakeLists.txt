add_executable(server server.c)
add_executable(client client.c)

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

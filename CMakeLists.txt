cmake_minimum_required(VERSION 3.22)

project(udx C)

add_subdirectory(vendor/libuv EXCLUDE_FROM_ALL)

add_library(udx STATIC)

target_sources(
  udx
  INTERFACE
    include/udx.h
    include/udx/cirbuf.h
    include/udx/fifo.h
  PRIVATE
    src/cirbuf.c
    src/fifo.c
    src/io.h
    src/udx.c
)

if (UNIX)
  target_sources(
    udx
    PRIVATE
      src/io_posix.c
  )
endif()

target_link_libraries(
  udx
  PRIVATE
    uv
)

install(TARGETS udx)

install(
  DIRECTORY include/
  DESTINATION include
  FILES_MATCHING PATTERN "*.h"
)

cmake_minimum_required(VERSION 3.22)

project(udx C)

add_subdirectory(vendor/libuv EXCLUDE_FROM_ALL)

add_library(udx OBJECT)

set_target_properties(
  udx
  PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE 1
)

target_sources(
  udx
  INTERFACE
    include/udx.h
  PRIVATE
    src/cirbuf.h
    src/cirbuf.c
    src/endian.h
    src/endian.c
    src/fifo.h
    src/fifo.c
    src/io.h
    src/udx.c
)

target_include_directories(
  udx
  PUBLIC
    include
    $<TARGET_PROPERTY:uv,INCLUDE_DIRECTORIES>
)

if (UNIX)
  target_sources(
    udx
    PRIVATE
      src/io_posix.c
  )
endif()

if (WIN32)
  target_sources(
    udx
    PRIVATE
      src/io_win.c
  )
endif()

add_library(udx_shared SHARED $<TARGET_OBJECTS:udx>)

set_target_properties(
  udx_shared
  PROPERTIES
  OUTPUT_NAME udx
)

target_link_libraries(
  udx_shared
  PUBLIC
    uv
  PRIVATE
    m
)

add_library(udx_static STATIC $<TARGET_OBJECTS:udx>)

set_target_properties(
  udx_static
  PROPERTIES
  OUTPUT_NAME udx
)

target_link_libraries(
  udx_static
  PUBLIC
    uv_a
  PRIVATE
    m
)

install(TARGETS udx_shared udx_static)

install(FILES include/udx.h DESTINATION include)

enable_testing()

add_subdirectory(test)

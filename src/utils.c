#include <stdlib.h>
#include <uv.h>

#include "../include/udx/utils.h"

uint64_t
udx_get_microseconds () {
  return uv_hrtime() / 1000;
}

uint64_t
udx_get_milliseconds () {
  return udx_get_microseconds() / 1000;
}

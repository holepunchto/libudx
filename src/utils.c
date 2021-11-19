#include <time.h>

#include "utils.h"

uint64_t
ucp_get_milliseconds() {
  return ucp_get_microseconds() / 1000;
}

uint64_t
ucp_get_microseconds() {
  struct timespec now;
  timespec_get(&now, TIME_UTC);
  uint64_t us =
      ((uint64_t)now.tv_sec) * 1000000 + ((uint64_t)now.tv_nsec) / 1000;
  static uint64_t epoch = 0;
  if (epoch == 0) {
    epoch = us;
  }
  return us - epoch;
}

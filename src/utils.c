// Modified from https://github.com/bittorrent/libutp/blob/master/utp_internal.cpp

#include <stdlib.h>
#include <stdint.h>
#include "utils.h"

#if defined(__APPLE__)
#include <mach/mach_time.h>

uint64_t
ucp_get_microseconds () {
  // http://developer.apple.com/mac/library/qa/qa2004/qa1398.html
  // http://www.macresearch.org/tutorial_performance_and_time
  static mach_timebase_info_data_t sTimebaseInfo;
  static uint64_t start_tick = 0;
  uint64_t tick;
  // Returns a counter in some fraction of a nanoseconds
  tick = mach_absolute_time();
  if (sTimebaseInfo.denom == 0) {
    // Get the timer ratio to convert mach_absolute_time to nanoseconds
    mach_timebase_info(&sTimebaseInfo);
    start_tick = tick;
  }
  // Calculate the elapsed time, convert it to microseconds and return it.
  return ((tick - start_tick) * sTimebaseInfo.numer) / (sTimebaseInfo.denom * 1000);
}

#else // !__APPLE__
#endif

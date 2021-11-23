// Modified from https://github.com/bittorrent/libutp/blob/master/utp_internal.cpp

#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
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
#include <unistd.h>

#if ! (defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC))
  #warning "Using non-monotonic function gettimeofday() in ucp_get_microseconds()"
#endif

/* Unfortunately, #ifdef CLOCK_MONOTONIC is not enough to make sure that
   POSIX clocks work -- we could be running a recent libc with an ancient
   kernel (think OpenWRT). -- jch */

uint64_t
ucp_get_microseconds () {
  struct timeval tv;

  #if defined(_POSIX_TIMERS) && _POSIX_TIMERS > 0 && defined(CLOCK_MONOTONIC)
  static int have_posix_clocks = -1;
  int rc;

  if (have_posix_clocks < 0) {
    struct timespec ts;
    rc = clock_gettime(CLOCK_MONOTONIC, &ts);
    have_posix_clocks = rc < 0 ? 0 : 1;
  }

   if (have_posix_clocks) {
     struct timespec ts;
     rc = clock_gettime(CLOCK_MONOTONIC, &ts);
     return (uint64_t) (ts.tv_sec) * 1000000 + (uint64_t) (ts.tv_nsec) / 1000;
   }
   #endif

   gettimeofday(&tv, NULL);
   return (uint64_t) (tv.tv_sec) * 1000000 + tv.tv_usec;
}
#endif

#ifndef UDX_DEBUG_H
#define UDX_DEBUG_H

#ifndef NDEBUG
#define DEBUG 1
#else
#define DEBUG 0
#endif

#ifdef DEBUG_STATS
#include <uv.h>
#include "../include/udx.h"

static uint64_t debug_start = 0;

static void
debug_print_cwnd_stats (udx_stream_t *stream) {
  if (!debug_start) debug_start = uv_hrtime() / 1000000;
  printf("%llu %u %u %u\n", (uv_hrtime() / 1000000) - debug_start, stream->cwnd, stream->cwnd_cnt, stream->srtt);
}
#else
static void
debug_print_cwnd_stats (udx_stream_t *stream) {}
#endif

#define debug_printf(...) \
  do { \
    if (DEBUG) fprintf(stderr, __VA_ARGS__); \
  } while (0)

#endif // UDX_DEBUG_H

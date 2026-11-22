
#ifndef UDX_WINDOWED_FILTER
#define UDX_WINDOWED_FILTER
#include "../include/udx.h"
#include <stdint.h>

static inline uint64_t
win_filter_get (win_filter_t *wf) {
  return wf->entries[0].v;
}

// t, win and v share no unit requirement beyond t and win matching each other.
// all three are 64 bit so that nanosecond timestamps, windows and values fit
uint64_t
win_filter_reset (win_filter_t *wf, uint64_t t, uint64_t value);
uint64_t
win_filter_apply_min (win_filter_t *wf, uint64_t win, uint64_t t, uint64_t v);
uint64_t
win_filter_apply_max (win_filter_t *wf, uint64_t win, uint64_t t, uint64_t v);

#endif // UDX_WINDOWED_FILTER

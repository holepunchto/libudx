
#ifndef UDX_WINDOWED_FILTER
#define UDX_WINDOWED_FILTER
#include "../include/udx.h"
#include <stdint.h>

static inline uint32_t
win_filter_get (win_filter_t *wf) {
  return wf->entries[0].v;
}

uint32_t
win_filter_reset (win_filter_t *wf, uint64_t t, uint32_t value);
uint32_t
win_filter_apply_min (win_filter_t *wf, uint32_t win, uint64_t t, uint32_t v);
uint32_t
win_filter_apply_max (win_filter_t *wf, uint32_t win, uint64_t t, uint32_t v);

#endif // UDX_WINDOWED_FILTER

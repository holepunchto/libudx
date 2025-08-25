
#ifndef UDX_WINDOWED_FILTER_F64
#define UDX_WINDOWED_FILTER_F64
#include "../include/udx.h"
#include <stdint.h>

static inline double
win_filter_f64_get (win_filter_f64_t *wf) {
  return wf->entries[0].v;
}

uint32_t
win_filter_f64_reset (win_filter_f64_t *wf, uint64_t t, double value);
uint32_t
win_filter_f64_apply_min (win_filter_f64_t *wf, uint32_t win, uint64_t t, double v);
uint32_t
win_filter_f64_apply_max (win_filter_f64_t *wf, uint32_t win, uint64_t t, double v);

#endif // UDX_WINDOWED_FILTER_F64

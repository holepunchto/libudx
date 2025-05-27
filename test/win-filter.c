
#include "../include/udx.h"
#include "../src/win_filter.h"
#include <assert.h>

int
main () {
  win_filter_t min_filter;
  win_filter_t max_filter;

  uint32_t win_ms = 300 * 1000;
  uint64_t now_ms = 0;

  win_filter_reset(&min_filter, now_ms, ~0U);
  win_filter_reset(&max_filter, now_ms, 0);

  uint32_t rtt = 0;
  uint64_t t = now_ms;

  // monotonically increasing rtt
  for (; t < win_ms; t++, rtt++) {
    win_filter_apply_min(&min_filter, win_ms, t, rtt);
    win_filter_apply_max(&max_filter, win_ms, t, rtt);
  }

  printf("min=%u max=%u\n", win_filter_get(&min_filter), win_filter_get(&max_filter));

  assert(win_filter_get(&min_filter) == 0);
  assert(win_filter_get(&max_filter) == 300 * 1000 - 1); // -1 because stopped before we actually reach win_ms

  // now add one more, reaching the min filter

  win_filter_apply_min(&min_filter, win_ms, t, rtt);
  win_filter_apply_max(&max_filter, win_ms, t, rtt);

  assert(win_filter_get(&min_filter) == 0);
  assert(win_filter_get(&max_filter) == 300 * 1000);

  printf("min=%u max=%u\n", win_filter_get(&min_filter), win_filter_get(&max_filter));

  // and one more, bumping off the min_filter
  t++;
  rtt++;
  win_filter_apply_min(&min_filter, win_ms, t, rtt);
  win_filter_apply_max(&max_filter, win_ms, t, rtt);

  assert(win_filter_get(&min_filter) == 75001);
  assert(win_filter_get(&max_filter) == 300 * 1000 + 1);

  printf("min=%u max=%u\n", win_filter_get(&min_filter), win_filter_get(&max_filter));

  // stop monotonically increasing, add a new realistic '14ms' sample
  // add it 3 times for good measure..

  t++;
  win_filter_apply_min(&min_filter, win_ms, t, 14);
  win_filter_apply_max(&min_filter, win_ms, t, 14);

  t++;
  win_filter_apply_min(&min_filter, win_ms, t, 14);
  win_filter_apply_max(&min_filter, win_ms, t, 14);

  assert(win_filter_get(&min_filter) == 14);
  assert(win_filter_get(&max_filter) == 300 * 1000 + 1);

  return 0;
}

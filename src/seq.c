#include "seq.h"

uint32_t
udx__seq_add (uint32_t a, uint32_t b) {
  return a + b;
}

uint32_t
udx__seq_inc (uint32_t seq) {
  return seq + 1;
}

int32_t
udx__seq_diff (uint32_t a, uint32_t b) {
  return a - b;
}

int
udx__seq_compare (uint32_t a, uint32_t b) {
  int32_t d = udx__seq_diff(a, b);
  return d < 0 ? -1 : d > 0 ? 1 : 0;
}
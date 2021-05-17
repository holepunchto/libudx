#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "fifo.h"

void
ucp_fifo_init (ucp_fifo_t *f, uint32_t initial_max_size) {
  // OBS: initial_max_size MUST be 2^n
  f->values = (void **) malloc(initial_max_size * sizeof(void *));
  f->mask = initial_max_size - 1;
  f->max_len = initial_max_size;
  f->len = 0;
  f->btm = 0;
}

void
ucp_fifo_destroy (ucp_fifo_t *f) {
  free(f->values);
  f->values = NULL;
}

void *
ucp_fifo_shift (ucp_fifo_t *f) {
  if (f->len == 0) return NULL;

  uint32_t btm = f->btm;
  void **b = f->values + btm;
  f->btm = (btm + 1) & f->mask;
  f->len--;

  return *b;
}

void
ucp_fifo_grow (ucp_fifo_t *f) {
  uint32_t mask = 2 * f->mask + 1;

  f->mask = mask;
  f->max_len = mask + 1;
  f->values = (void **) realloc(f->values, f->max_len * sizeof(void *));

  for (uint32_t i = 0; i < f->btm; i++) {
    f->values[f->len + i] = f->values[i];
  }
}

void
ucp_fifo_push (ucp_fifo_t *f, void *data) {
  if (f->len == f->max_len) ucp_fifo_grow(f);

  void **t = f->values + ((f->btm + f->len++) & f->mask);
  *t = data;
}

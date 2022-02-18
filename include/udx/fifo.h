#ifndef UDX_FIFO_H
#define UDX_FIFO_H

#include <stdlib.h>

typedef struct {
  uint32_t btm;
  uint32_t len;
  uint32_t max_len;
  uint32_t mask;
  void **values;
} udx_fifo_t;

void
udx_fifo_init (udx_fifo_t *f, uint32_t initial_max_size);

void
udx_fifo_destroy (udx_fifo_t *f);

void *
udx_fifo_shift (udx_fifo_t *f);

void
udx_fifo_grow (udx_fifo_t *f);

uint32_t
udx_fifo_push (udx_fifo_t *f, void *data);

void
udx_fifo_remove (udx_fifo_t *f, void *data, uint32_t pos_hint);

#endif // UDX_FIFO_H

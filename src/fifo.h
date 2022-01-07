#ifndef UCP_FIFO_H
#define UCP_FIFO_H

#include <stdlib.h>
#include <stdint.h>

typedef struct {
  uint32_t btm;
  uint32_t len;
  uint32_t max_len;
  uint32_t mask;
  void **values;
} ucp_fifo_t;

void
ucp_fifo_init (ucp_fifo_t *f, uint32_t initial_max_size);

void
ucp_fifo_destroy (ucp_fifo_t *f);

void *
ucp_fifo_shift (ucp_fifo_t *f);

void
ucp_fifo_grow (ucp_fifo_t *f);

uint32_t
ucp_fifo_push (ucp_fifo_t *f, void *data);

void
ucp_fifo_remove (ucp_fifo_t *f, void *data, uint32_t pos_hint);

#endif

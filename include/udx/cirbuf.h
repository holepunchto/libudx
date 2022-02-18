#ifndef UDX_CIRBUF_H
#define UDX_CIRBUF_H

#include <stdlib.h>
#include <stdint.h>

typedef struct {
  uint32_t seq;
} udx_cirbuf_val_t;

typedef struct {
  uint32_t size;
  uint32_t mask;
  udx_cirbuf_val_t **values;
} udx_cirbuf_t;

void
udx_cirbuf_init (udx_cirbuf_t *c, uint32_t initial_size);

void
udx_cirbuf_destroy (udx_cirbuf_t *c);

void
udx_cirbuf_set (udx_cirbuf_t *c, udx_cirbuf_val_t *val);

udx_cirbuf_val_t *
udx_cirbuf_get (udx_cirbuf_t *c, uint32_t seq);

udx_cirbuf_val_t *
udx_cirbuf_get_stored (udx_cirbuf_t *c, uint32_t seq);

udx_cirbuf_val_t *
udx_cirbuf_remove (udx_cirbuf_t *c, uint32_t seq);

#endif // UDX_CIRBUF_H

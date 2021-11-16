#ifndef UCP_CIRBUF
#define UCP_CIRBUF

#include <stdlib.h>

typedef struct {
  uint32_t seq;
} ucp_cirbuf_val_t;

typedef struct {
  uint32_t size;
  uint32_t mask;
  ucp_cirbuf_val_t **values;
} ucp_cirbuf_t;

void
ucp_cirbuf_init (ucp_cirbuf_t *c, uint32_t initial_size);

void
ucp_cirbuf_destroy (ucp_cirbuf_t *c);

void
ucp_cirbuf_set (ucp_cirbuf_t *c, ucp_cirbuf_val_t *val);

ucp_cirbuf_val_t *
ucp_cirbuf_get (ucp_cirbuf_t *c, uint32_t seq);

ucp_cirbuf_val_t *
ucp_cirbuf_get_stored (ucp_cirbuf_t *c, uint32_t seq);

ucp_cirbuf_val_t *
ucp_cirbuf_remove (ucp_cirbuf_t *c, uint32_t seq);

#endif

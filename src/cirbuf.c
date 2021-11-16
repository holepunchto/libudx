#include "cirbuf.h"
#include <stdlib.h>

void
ucp_cirbuf_init (ucp_cirbuf_t *c, uint32_t initial_size) {
  // OBS: initial_size MUST be 2^n
  c->size = initial_size;
  c->mask = initial_size - 1;
  c->values = calloc(initial_size, sizeof(ucp_cirbuf_val_t *));
}

void
ucp_cirbuf_destroy (ucp_cirbuf_t *c) {
  if (c->values != NULL) free(c->values);
  c->size = c->mask = 0;
  c->values = NULL;
}

void
ucp_cirbuf_set (ucp_cirbuf_t *c, ucp_cirbuf_val_t *val) {
  ucp_cirbuf_val_t **values = c->values + (val->seq & c->mask);
  ucp_cirbuf_val_t *v = *values;

  if (v == NULL || v->seq == val->seq) {
    *values = val;
    return;
  }

  uint32_t old_size = c->size;
  ucp_cirbuf_val_t **old_values = c->values;

  while ((v->seq & c->mask) == (val->seq & c->mask)) {
    c->size *= 2;
    c->mask = c->size - 1;
  }

  c->values = calloc(c->size, sizeof(ucp_cirbuf_val_t *));
  c->values[val->seq & c->mask] = val;
  for (uint32_t i = 0; i < old_size; i++) {
    ucp_cirbuf_val_t *v = old_values[i];
    if (v != NULL) c->values[v->seq & c->mask] = v;
  }

  free(old_values);
}

ucp_cirbuf_val_t *
ucp_cirbuf_get (ucp_cirbuf_t *c, uint32_t seq) {
  ucp_cirbuf_val_t *v = c->values[seq & c->mask];
  return (v == NULL || v->seq != seq) ? NULL : v;
}

ucp_cirbuf_val_t *
ucp_cirbuf_get_stored (ucp_cirbuf_t *c, uint32_t seq) {
  return c->values[seq & c->mask];
}

ucp_cirbuf_val_t *
ucp_cirbuf_remove (ucp_cirbuf_t *c, uint32_t seq) {
  ucp_cirbuf_val_t **values = c->values + (seq & c->mask);
  ucp_cirbuf_val_t *v = *values;

  if (v == NULL || v->seq != seq) return NULL;

  *values = NULL;
  return v;
}

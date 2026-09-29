
#define UDX_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#include "../include/udx.h"

static udx_stream_entry_t **
lookup (udx_t *udx, uint32_t local_id) {
  int i = local_id % UDX_ARRAY_SIZE(udx->stream_table);

  udx_stream_entry_t **p = &udx->stream_table[i];

  while (*p) {
    if ((*p)->local_id == local_id) {
      return p;
    }
    p = &(*p)->hash_next;
  }
  return p;
}

udx_stream_entry_t *
udx_stream_entry_get (udx_t *udx, uint32_t id) {
  return *lookup(udx, id);
}

// return entry that was replaced or NULL if no prior entry
udx_stream_entry_t *
udx_stream_entry_set (udx_t *udx, udx_stream_entry_t *new) {
  uint32_t id = new->local_id;
  udx_stream_entry_t **p = lookup(udx, id);
  if (*p) {
    udx_stream_entry_t *old = *p;
    new->hash_next = old->hash_next;
    *p = new;
    return old;
  }
  *p = new;
  new->hash_next = NULL;

  return NULL;
}

udx_stream_entry_t *
udx_stream_entry_remove (udx_t *udx, uint32_t id) {
  udx_stream_entry_t **p = lookup(udx, id);

  udx_stream_entry_t *ret = *p;
  if (*p) {
    *p = (*p)->hash_next;
  }

  return ret;
}

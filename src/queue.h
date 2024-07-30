#ifndef UDX_QUEUE_H
#define UDX_QUEUE_H

#include "../include/udx.h"

#define udx__queue_data(pointer, type, field) \
  ((type *) ((char *) (pointer) -offsetof(type, field)))

#define udx__queue_foreach(q, h) \
  for ((q) = (h)->next; (q) != (h); (q) = (q)->next)

void
udx__queue_init (udx_queue_t *q);
void
udx__queue_head (udx_queue_t *q, udx_queue_node_t *pkt); // unshift
void
udx__queue_tail (udx_queue_t *q, udx_queue_node_t *pkt); // push

void
udx__queue_unlink (udx_queue_t *q, udx_queue_node_t *pkt);

udx_queue_node_t *
udx__queue_peek (udx_queue_t *q);

udx_queue_node_t *
udx__queue_shift (udx_queue_t *q);
#endif // UDX_QUEUE_H

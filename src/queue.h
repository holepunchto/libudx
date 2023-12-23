#ifndef UDX_QUEUE_H
#define UDX_QUEUE_H

#include "../include/udx.h"

void
udx__queue_init (udx_queue_t *q);
void
udx__queue_head (udx_queue_t *q, udx_packet_t *pkt); // unshift
void
udx__queue_tail (udx_queue_t *q, udx_packet_t *pkt); // push

void
udx__queue_unlink (udx_queue_t *q, udx_packet_t *pkt);

udx_packet_t *
udx__queue_peek (udx_queue_t *q);

udx_packet_t *
udx__queue_shift (udx_queue_t *q);
#endif // UDX_QUEUE_H

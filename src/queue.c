#include "../include/udx.h"
#include "queue.h"

void
udx__queue_init (udx_queue_t *q) {
  q->prev = (udx_packet_t *) q;
  q->next = (udx_packet_t *) q;
  q->len = 0;
}

// all insertion operations call this general version
static void
queue_insert (udx_packet_t *pkt, udx_packet_t *prev, udx_packet_t *next, udx_queue_t *queue) {
  if (pkt == NULL) __builtin_trap();
  if (prev == NULL) __builtin_trap();
  if (next == NULL) __builtin_trap();
  pkt->next = next;
  pkt->prev = prev;
  next->prev = pkt;
  prev->next = pkt;

  queue->len++;
}

static void
queue_after (udx_queue_t *queue, udx_packet_t *prev, udx_packet_t *pkt) {
  queue_insert(pkt, prev, prev->next, queue);
}

static void
queue_before (udx_queue_t *queue, udx_packet_t *next, udx_packet_t *pkt) {
  queue_insert(pkt, next->prev, next, queue);
}

void
udx__queue_head (udx_queue_t *q, udx_packet_t *pkt) {
  queue_after(q, (udx_packet_t *) q, pkt);
}

void
udx__queue_tail (udx_queue_t *q, udx_packet_t *pkt) { // todo: queue_push
  queue_before(q, (udx_packet_t *) q, pkt);
}

void
udx__queue_unlink (udx_queue_t *q, udx_packet_t *pkt) {
  udx_packet_t *next;
  udx_packet_t *prev;

  q->len--;

  next = pkt->next;
  prev = pkt->prev;
  pkt->next = NULL;
  pkt->prev = NULL;

  next->prev = prev;
  prev->next = next;
}

udx_packet_t *
udx__queue_peek (udx_queue_t *q) {
  udx_packet_t *pkt = q->next;
  if ((udx_queue_t *) pkt == q) {
    pkt = NULL;
  }
  return pkt;
}

udx_packet_t *
udx__queue_shift (udx_queue_t *q) { // udx__queue_dequeue
  udx_packet_t *pkt = udx__queue_peek(q);

  if (pkt) {
    udx__queue_unlink(q, pkt);
  }
  return pkt;
}

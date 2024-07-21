#include "../include/udx.h"
#include "assert.h"
#include "queue.h"

void
udx__queue_init (udx_queue_t *q) {

  q->node.prev = &q->node;
  q->node.next = &q->node;
  q->len = 0;
}

// all insertion operations call this general version
static void
queue_insert (udx_queue_node_t *new, udx_queue_node_t *prev, udx_queue_node_t *next, udx_queue_t *queue) {
  new->next = next;
  new->prev = prev;
  next->prev = new;
  prev->next = new;

  queue->len++;
}

static void
queue_after (udx_queue_t *q, udx_queue_node_t *prev, udx_queue_node_t *new) {
  queue_insert(new, prev, prev->next, q);
}

static void
queue_before (udx_queue_t *q, udx_queue_node_t *next, udx_queue_node_t *new) {
  queue_insert(new, next->prev, next, q);
}

void
udx__queue_head (udx_queue_t *q, udx_queue_node_t *new) {
  queue_after(q, (udx_queue_node_t *) q, new);
}

void
udx__queue_tail (udx_queue_t *q, udx_queue_node_t *new) {
  queue_before(q, (udx_queue_node_t *) q, new);
}

// assumes p must be a member of q
void
udx__queue_unlink (udx_queue_t *q, udx_queue_node_t *p) {
  assert(q->len != 0);
  q->len--;

  p->prev->next = p->next;
  p->next->prev = p->prev;
  p->next = NULL;
  p->prev = NULL;
}

udx_queue_node_t *
udx__queue_peek (udx_queue_t *q) {
  udx_queue_node_t *ret = q->node.next;
  if (ret == &q->node) {
    ret = NULL;
  }
  return ret;
}

udx_queue_node_t *
udx__queue_shift (udx_queue_t *q) { // udx__queue_dequeue
  udx_queue_node_t *ret = udx__queue_peek(q);

  if (ret) {
    udx__queue_unlink(q, ret);
  }
  return ret;
}

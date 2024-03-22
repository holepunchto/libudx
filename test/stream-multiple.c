#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "helpers.h"

#define NBYTES_TO_SEND 10000000
#define NSTREAMS       16

uv_loop_t loop;
udx_t udx;

struct sender {
  struct sockaddr_in addr;
  udx_socket_t usock;
  udx_stream_t stream;
  udx_stream_write_t *write;
  // size_t nbytes_written;
  size_t write_hash;

  bool ack;
} sender[NSTREAMS];

struct receiver {
  struct sockaddr_in addr;
  udx_socket_t usock;
  udx_stream_t stream;

  size_t nbytes_read;
  size_t read_hash;
} receiver[NSTREAMS];

static bool
all_acked () {
  for (int i = 0; i < NSTREAMS; i++) {
    if (sender[i].ack == false) {
      return false;
    }
  }
  return true;
}

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  assert(status == 0);

  struct sender *s = &sender[r->stream->local_id];
  s->ack = true;

  if (all_acked()) {
    uv_stop(&loop);
  }
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  struct receiver *r = &receiver[handle->local_id - NSTREAMS];

  r->nbytes_read += read_len;
  r->read_hash = hash(r->read_hash, buf->base, read_len);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init(malloc(NBYTES_TO_SEND), NBYTES_TO_SEND);

  size_t write_hash = HASH_INIT;

  write_hash = hash(write_hash, buf.base, buf.len);

  for (int i = 0; i < NSTREAMS; i++) {
    int sender_id = i;
    int receiver_id = NSTREAMS + i;

    receiver[i].read_hash = HASH_INIT;
    e = udx_socket_init(&udx, &sender[i].usock);
    assert(e == 0);
    uv_ip4_addr("127.0.0.1", 8000 + i, &sender[i].addr);
    e = udx_socket_bind(&sender[i].usock, (struct sockaddr *) &sender[i].addr, 0);
    assert(e == 0);
    sender[i].write = malloc(udx_stream_write_sizeof(1));
    e = udx_stream_init(&udx, &sender[i].stream, sender_id, NULL);

    udx_socket_init(&udx, &receiver[i].usock);
    uv_ip4_addr("127.0.0.1", 8100 + i, &receiver[i].addr);
    e = udx_socket_bind(&receiver[i].usock, (struct sockaddr *) &receiver[i].addr, 0);
    assert(e == 0);
    e = udx_stream_init(&udx, &receiver[i].stream, receiver_id, NULL);
    assert(e == 0);

    e = udx_stream_read_start(&receiver[i].stream, on_read);
    assert(e == 0);
    sender[i].write_hash = write_hash;

    e = udx_stream_connect(&sender[i].stream, &sender[i].usock, receiver_id, (struct sockaddr *) &receiver[i].addr);
    assert(e == 0);

    e = udx_stream_connect(&receiver[i].stream, &receiver[i].usock, sender_id, (struct sockaddr *) &sender[i].addr);
    assert(e == 0);

    udx_stream_write(sender[i].write, &sender[i].stream, &buf, 1, on_ack);
  }

  uv_run(&loop, UV_RUN_DEFAULT);

  for (int i = 0; i < NSTREAMS; i++) {
    printf("%d: send_hash=%lx receive_hash=%lx sent_bytes=%d recv_bytes=%lu\n", i, sender[i].write_hash, receiver[i].read_hash, NBYTES_TO_SEND, receiver[i].nbytes_read);
    assert(sender[i].write_hash == receiver[i].read_hash);
    assert(receiver[i].nbytes_read == NBYTES_TO_SEND);
  }

  return 0;
}

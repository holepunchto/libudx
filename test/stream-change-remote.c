#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "helpers.h"

#define NBYTES_TO_SEND 1000000

uv_loop_t loop;
udx_t udx;

struct sockaddr_in aaddr;
udx_socket_t asock;
udx_stream_t astream;

struct sockaddr_in baddr;
udx_socket_t bsock;
udx_stream_t bstream;

struct sockaddr_in caddr;
udx_socket_t csock;
udx_stream_t cstream;

struct sockaddr_in daddr;
udx_socket_t dsock;
udx_stream_t dstream;

bool ack_called = false;
bool read_called = false;
int remote_changed_called = 0;

size_t nbytes_read;

size_t read_hash = HASH_INIT;
size_t write_hash = HASH_INIT;

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  uv_stop(&loop);

  ack_called = true;
}

void
on_remote_change (udx_stream_t *s) {
  remote_changed_called++;

  if (remote_changed_called == 2) {
    int e;

    e = udx_stream_destroy(&bstream);
    assert(e == 0);

    e = udx_stream_destroy(&cstream);
    assert(e == 0);
  }
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  int e;

  static bool changed = false;

  nbytes_read += read_len;

  read_hash = hash(read_hash, buf->base, read_len);

  // swap to relay 1/3 of the way into the stream

  if (nbytes_read > (NBYTES_TO_SEND / 3) && !changed) {
    e = udx_stream_change_remote(&astream, &bsock, 4, (struct sockaddr *) &daddr, on_remote_change);
    assert(e == 0 && "reconnect");

    e = udx_stream_change_remote(&dstream, &csock, 1, (struct sockaddr *) &aaddr, on_remote_change);
    assert(e == 0 && "reconnect");

    changed = true;
  }

  read_called = true;
}

int
main () {
  int e;

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx);
  assert(e == 0);

  e = udx_socket_init(&udx, &asock);
  assert(e == 0);

  e = udx_socket_init(&udx, &bsock);
  assert(e == 0);

  e = udx_socket_init(&udx, &csock);
  assert(e == 0);

  e = udx_socket_init(&udx, &dsock);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 8083, &caddr);
  e = udx_socket_bind(&csock, (struct sockaddr *) &caddr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 8084, &daddr);
  e = udx_socket_bind(&dsock, (struct sockaddr *) &daddr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, NULL, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, NULL, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &cstream, 3, NULL, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &dstream, 4, NULL, NULL);
  assert(e == 0);

  // a <-> b  <-relay-> c <-> d

  e = udx_stream_relay_to(&cstream, &bstream);
  assert(e == 0);

  e = udx_stream_relay_to(&bstream, &cstream);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_connect(&cstream, &csock, 4, (struct sockaddr *) &daddr);
  assert(e == 0);

  e = udx_stream_connect(&dstream, &dsock, 3, (struct sockaddr *) &caddr);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init(malloc(NBYTES_TO_SEND), NBYTES_TO_SEND);

  write_hash = hash(write_hash, buf.base, buf.len);

  e = udx_stream_write(req, &dstream, &buf, 1, on_ack);
  assert(e == 0); // write bigger than hwm

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(ack_called && read_called && remote_changed_called && nbytes_read == NBYTES_TO_SEND);

  assert(read_hash == write_hash);
  printf("read_hash=%lu write_hash=%lu\n", read_hash, write_hash);

  free(req);

  return 0;
}

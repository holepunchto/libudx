/*
 * for this test to pass, you must relay 127.0.0.2:8082 to 127.0.0.1:8082
 *
 * this python script works:
 *
 * r = socket(AF_INET, SOCK_DGRAM)
 * r.bind(("127.0.0.2", 8082))
 *
 * s = socket(AF_INET, SOCK_DGRAM)
 * s.connect(("127.0.0.1", 8082))
 *
 * while True:
 *     buf = r.recv(2000)
 *     s.send(buf)
 */

#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_stream_t astream;

udx_socket_t bsock;
udx_stream_t bstream;

udx_stream_write_t req;

bool ack_called;
bool read_called;

#define NBYTES_TO_SEND 100000

size_t nbytes_read;

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  printf("on_ack\n");

  uv_stop(&loop);

  ack_called = true;
}

void
on_remote_change (udx_stream_t *s) {
  printf("on_remote_change!\n");
}

bool changed = false;

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  // printf("on_read, bytes=%ld local_id=%d remote_id=%d\n", read_len, handle->local_id, handle->remote_id);

  nbytes_read += read_len;

  // swap to relay 1/3 of the way into the stream

  if (nbytes_read > (NBYTES_TO_SEND / 3) && !changed) {
    struct sockaddr_in raddr;
    uv_ip4_addr("127.0.0.2", 8082, &raddr);

    int e = udx_stream_change_remote(&bstream, 1, (struct sockaddr *) &raddr, on_remote_change);
    assert(e == 0 && "reconnect");

    changed = true;
  }

  read_called = true;
}

int
main () {

  uv_loop_init(&loop);

  int e = udx_init(&loop, &udx);

  e = udx_socket_init(&udx, &asock);
  assert(e == 0);

  e = udx_socket_init(&udx, &bsock);
  assert(e == 0);

  struct sockaddr_in baddr;
  uv_ip4_addr("127.0.0.1", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr, 0);
  assert(e == 0);

  struct sockaddr_in aaddr;
  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, NULL);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init(malloc(NBYTES_TO_SEND), NBYTES_TO_SEND);
  udx_stream_write(&req, &bstream, &buf, 1, on_ack);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(ack_called && read_called);
  assert(nbytes_read == NBYTES_TO_SEND);

  return 0;
}

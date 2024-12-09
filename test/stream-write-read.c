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

udx_stream_write_t *req;

bool ack_called = false;
bool read_called = false;
bool eof_received = false;

int nclosed;

void
on_close (udx_stream_t *s, int status) {
  assert(status == 0);

  nclosed++;

  if (nclosed == 2) {
    udx_socket_close(&asock);
    udx_socket_close(&bsock);
  }
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  assert(unordered == 0);

  ack_called = true;
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {

  if (read_len == UV_EOF) {
    eof_received = true;
    return;
  }

  assert(buf->len == 5);
  assert(buf->len == read_len);
  assert(memcmp(buf->base, "hello", 5) == 0);

  read_called = true;
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &asock, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &bsock, NULL);
  assert(e == 0);

  struct sockaddr_in baddr;
  uv_ip4_addr("127.0.0.1", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr, 0);
  assert(e == 0);

  struct sockaddr_in aaddr;
  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init("hello", 5);
  e = udx_stream_write(req, &bstream, &buf, 1, on_ack);
  assert(e && "drained");

  udx_stream_write_t *end_request_a = malloc(udx_stream_write_sizeof(1));
  udx_stream_write_t *end_request_b = malloc(udx_stream_write_sizeof(1));

  e = udx_stream_write_end(end_request_a, &astream, NULL, 0, NULL);
  assert(e);
  e = udx_stream_write_end(end_request_b, &bstream, NULL, 0, NULL);
  assert(e);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  e = uv_loop_close(&loop);
  assert(e == 0);

  free(end_request_a);
  free(end_request_b);
  free(req);

  assert(ack_called && read_called && eof_received);

  return 0;
}

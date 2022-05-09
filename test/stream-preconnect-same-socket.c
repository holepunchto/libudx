#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;
struct sockaddr_in addr;
udx_socket_t sock;

udx_stream_t astream;
udx_stream_t bstream;

udx_stream_write_t req;

bool ack_called = false;
bool read_called = false;

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  assert(&req == r);
  assert(status == 0);
  assert(unordered == 0);

  uv_stop(&loop);

  ack_called = true;
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  assert(buf->len == 5);
  assert(buf->len == read_len);
  assert(memcmp(buf->base, "hello", 5) == 0);

  read_called = true;

  int e = udx_stream_connect(&astream, &sock, 2, (struct sockaddr *) &addr, NULL);
  assert(e == 0);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 8081, &addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &addr);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &sock, 1, (struct sockaddr *) &addr, NULL);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init("hello", 5);
  udx_stream_write(&req, &bstream, &buf, 1, on_ack);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(ack_called && read_called);

  return 0;
}

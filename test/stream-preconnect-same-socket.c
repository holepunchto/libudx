#include <assert.h>

#include "../include/udx.h"

uv_loop_t loop;

udx_t sock;

udx_stream_t astream;
udx_stream_t bstream;

udx_stream_write_t req;

int ack_called = FALSE;
int read_called = FALSE;
int preconnect_called = FALSE;

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  assert(&req == r);
  assert(status == 0);
  assert(unordered == 0);

  uv_stop(&loop);

  ack_called = TRUE;
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  assert(buf->len == 5);
  assert(buf->len == read_len);
  assert(memcmp(buf->base, "hello", 5) == 0);

  read_called = TRUE;
}

void
on_preconnect (udx_t *sock, uint32_t id, struct sockaddr *addr) {
  int e;

  e = udx_stream_connect(&astream, sock, 2, addr, NULL);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  preconnect_called = TRUE;
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &sock);
  assert(e == 0);

  struct sockaddr_in addr;
  uv_ip4_addr("127.0.0.1", 8081, &addr);
  e = udx_bind(&sock, (struct sockaddr *) &addr);
  assert(e == 0);

  e = udx_stream_init(&loop, &astream, 1);
  assert(e == 0);

  e = udx_stream_init(&loop, &bstream, 2);
  assert(e == 0);

  e = udx_preconnect(&sock, on_preconnect);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &sock, 1, (struct sockaddr *) &addr, NULL);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init("hello", 5);
  udx_stream_write(&req, &bstream, &buf, 1, on_ack);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(ack_called && read_called && preconnect_called);
}

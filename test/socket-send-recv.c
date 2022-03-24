#include <assert.h>

#include "../include/udx.h"

uv_loop_t loop;

udx_t asock;
udx_t bsock;

udx_send_t req;

int send_called = FALSE;
int recv_called = FALSE;

void
on_send (udx_send_t *r, int status) {
  assert(&req == r);
  assert(status == 0);

  send_called = TRUE;
}

void
on_recv (udx_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  assert(buf->len == 5);
  assert(buf->len == read_len);
  assert(memcmp(buf->base, "hello", 5) == 0);

  uv_stop(&loop);

  recv_called = TRUE;
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &asock);
  assert(e == 0);

  e = udx_init(&loop, &bsock);
  assert(e == 0);

  struct sockaddr_in baddr;
  uv_ip4_addr("127.0.0.1", 8082, &baddr);
  e = udx_bind(&bsock, (struct sockaddr *) &baddr);
  assert(e == 0);

  struct sockaddr_in aaddr;
  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_bind(&asock, (struct sockaddr *) &aaddr);
  assert(e == 0);

  udx_recv_start(&asock, on_recv);

  uv_buf_t buf = uv_buf_init("hello", 5);
  udx_send(&req, &bsock, &buf, 1, (struct sockaddr *) &aaddr, on_send);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(send_called && recv_called);
}

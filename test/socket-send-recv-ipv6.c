#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_socket_t bsock;

udx_socket_send_t req;

bool send_called = false;
bool recv_called = false;

void
on_send (udx_socket_send_t *r, int status) {
  assert(&req == r);
  assert(status == 0);

  send_called = true;
}

void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  assert(buf->len == 5);
  assert(buf->len == read_len);
  assert(memcmp(buf->base, "hello", 5) == 0);

  uv_stop(&loop);

  recv_called = true;
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx);
  assert(e == 0);

  e = udx_socket_init(&udx, &asock);
  assert(e == 0);

  e = udx_socket_init(&udx, &bsock);
  assert(e == 0);

  struct sockaddr_in6 baddr;
  uv_ip6_addr("::1", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr);
  assert(e == 0);

  struct sockaddr_in6 aaddr;
  uv_ip6_addr("::1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr);
  assert(e == 0);

  udx_socket_recv_start(&asock, on_recv);

  uv_buf_t buf = uv_buf_init("hello", 5);
  udx_socket_send(&req, &bsock, &buf, 1, (struct sockaddr *) &aaddr, on_send);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(send_called && recv_called);

  return 0;
}

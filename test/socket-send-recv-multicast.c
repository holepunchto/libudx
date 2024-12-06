
#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

#define MULTICAST_ADDR "239.255.0.1"

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_socket_t bsock;
udx_socket_send_t req;

int nclient_received;
int nserver_sent;

bool close_called;

void
on_send (udx_socket_send_t *r, int status) {
  assert(&req == r);
  assert(status == 0);

  nserver_sent++;
}

void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  assert(buf->len == 5);
  assert(buf->len == read_len);
  assert(memcmp(buf->base, "hello", 5) == 0);

  int e = udx_socket_set_membership(&asock, MULTICAST_ADDR, NULL, UV_LEAVE_GROUP);
  assert(e == 0);

  nclient_received++;
  uv_stop(&loop);
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

  struct sockaddr_in baddr;
  uv_ip4_addr("0.0.0.0", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr, 0);
  assert(e == 0);

  struct sockaddr_in aaddr;
  uv_ip4_addr("0.0.0.0", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr, UV_UDP_REUSEADDR);
  assert(e == 0);

  // e = udx_socket_set_multicast_loop(&asock, 1);
  // assert(e == 0);
  e = udx_socket_set_membership(&asock, MULTICAST_ADDR, NULL, UV_JOIN_GROUP);
  assert(e == 0);

  udx_socket_recv_start(&asock, on_recv);

  struct sockaddr_in mcast_addr;
  uv_ip4_addr(MULTICAST_ADDR, 8081, &mcast_addr);

  uv_buf_t buf = uv_buf_init("hello", 5);
  // udx_socket_send(&req, &bsock, &buf, 1, (struct sockaddr *) &aaddr, on_send);

  udx_socket_send(&req, &bsock, &buf, 1, (struct sockaddr *) &mcast_addr, on_send);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(nserver_sent && nclient_received);

  return 0;
}

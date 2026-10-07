#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

// block the loop for longer than the delivery timeout while an rto is pending,
// the stream must probe after the stall instead of timing out. the receiver
// only appears right before the stall, so the data is unacked when it starts

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

udx_stream_t send_stream;
udx_stream_t recv_stream;

uv_timer_t stall_timer;

int closed = 0;
bool acked = false;

void
on_close (udx_stream_t *stream, int status) {
  assert(status == 0);

  if (++closed == 2) {
    udx_socket_close(&send_sock);
    udx_socket_close(&recv_sock);
  }
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  acked = true;
  free(req);

  udx_stream_destroy(&send_stream);
  udx_stream_destroy(&recv_stream);
}

void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {}

void
on_stall (uv_timer_t *timer) {
  int e = udx_stream_init(&udx, &recv_stream, 2, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&recv_stream, &recv_sock, 1, (struct sockaddr *) &send_addr);
  assert(e == 0);

  e = udx_stream_read_start(&recv_stream, on_read);
  assert(e == 0);

  uv_sleep(4500);
}

void
bind_any (udx_socket_t *socket, struct sockaddr_in *addr) {
  int e = udx_socket_init(&udx, socket, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 0, addr);
  e = udx_socket_bind(socket, (struct sockaddr *) addr, 0);
  assert(e == 0);

  int len = sizeof(*addr);
  e = udx_socket_getsockname(socket, (struct sockaddr *) addr, &len);
  assert(e == 0);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  bind_any(&send_sock, &send_addr);
  bind_any(&recv_sock, &recv_addr);

  e = udx_stream_init(&udx, &send_stream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&send_stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_stream_set_delivery_timeout(&send_stream, 3000);
  assert(e == 0);

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  uv_buf_t buf = uv_buf_init("hello", 5);
  e = udx_stream_write(req, &send_stream, &buf, 1, on_ack);
  assert(e);

  // the rto fires at 1000 ms, the next one is due at 2000 ms, during the stall
  uv_timer_init(&loop, &stall_timer);
  uv_timer_start(&stall_timer, on_stall, 1500, 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  assert(acked);
  assert(closed == 2);

  return 0;
}

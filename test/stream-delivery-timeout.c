#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

// write to a socket that never acks:
// a: delivery timeout 100 ms, raised to 3 rtos, closes at 3000 ms
// b: default budget, set to 4000 ms at 3500 ms, the rto pending for 5000 ms is pulled in

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_socket_t sink;

udx_stream_t a;
udx_stream_t b;

uv_timer_t timer;

uint64_t start;
uint64_t a_closed;
uint64_t b_closed;

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == UV_ECANCELED);
  free(req);
}

void
on_close (udx_stream_t *stream, int status) {
  assert(status == UV_ETIMEDOUT);

  if (stream == &a) a_closed = uv_now(&loop) - start;
  else b_closed = uv_now(&loop) - start;

  if (a_closed && b_closed) {
    udx_socket_close(&sock);
    udx_socket_close(&sink);
  }
}

void
on_timer (uv_timer_t *timer) {
  int e = udx_stream_set_delivery_timeout(&b, 4000);
  assert(e == 0);
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

void
write_to_sink (udx_stream_t *stream, uint32_t local_id, struct sockaddr_in *sink_addr) {
  int e = udx_stream_init(&udx, stream, local_id, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(stream, &sock, local_id + 10, (struct sockaddr *) sink_addr);
  assert(e == 0);

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  uv_buf_t buf = uv_buf_init("hello", 5);
  e = udx_stream_write(req, stream, &buf, 1, on_ack);
  assert(e);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  struct sockaddr_in addr;
  bind_any(&sock, &addr);

  struct sockaddr_in sink_addr;
  bind_any(&sink, &sink_addr);

  uv_update_time(&loop);
  start = uv_now(&loop);

  write_to_sink(&a, 1, &sink_addr);
  e = udx_stream_set_delivery_timeout(&a, 100);
  assert(e == 0);

  write_to_sink(&b, 2, &sink_addr);

  uv_timer_init(&loop, &timer);
  uv_timer_start(&timer, on_timer, 3500, 0);
  uv_unref((uv_handle_t *) &timer);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  assert(a_closed >= 3000 && a_closed < 3500);
  assert(b_closed >= 4000 && b_closed < 4500);

  return 0;
}

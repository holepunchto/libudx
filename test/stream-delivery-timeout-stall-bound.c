#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

// a loop that blocks for more than two rtos on every iteration makes every
// timer late. the stalled time is not counted, but only until 6 rtos fired,
// so the stream must still time out when the peer is dead

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_socket_t sink;

udx_stream_t stream;

uv_timer_t lag_timer;

int lags = 0;
bool closed = false;

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == UV_ECANCELED);
  free(req);
}

void
on_close (udx_stream_t *s, int status) {
  assert(status == UV_ETIMEDOUT);
  closed = true;

  uv_close((uv_handle_t *) &lag_timer, NULL);
  udx_socket_close(&sock);
  udx_socket_close(&sink);
}

void
on_lag (uv_timer_t *timer) {
  assert(++lags < 12);
  uv_sleep(2300);
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

  struct sockaddr_in addr;
  bind_any(&sock, &addr);

  struct sockaddr_in sink_addr;
  bind_any(&sink, &sink_addr);

  e = udx_stream_init(&udx, &stream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &sink_addr);
  assert(e == 0);

  e = udx_stream_set_delivery_timeout(&stream, 3000);
  assert(e == 0);

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  uv_buf_t buf = uv_buf_init("hello", 5);
  e = udx_stream_write(req, &stream, &buf, 1, on_ack);
  assert(e);

  uv_timer_init(&loop, &lag_timer);
  uv_timer_start(&lag_timer, on_lag, 1, 1);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  assert(closed);
  assert(lags > 4); // the stalled time was skipped a few times before it timed out

  return 0;
}

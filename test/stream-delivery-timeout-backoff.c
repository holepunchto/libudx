#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

// a backed off rto must not stretch the default delivery budget, it stays 13 x
// the rto without backoff (1000 ms before an rtt sample)

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_socket_t sink;

udx_stream_t stream;

uint64_t start;

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == UV_ECANCELED);
  free(req);
}

void
on_close (udx_stream_t *s, int status) {
  assert(status == UV_ETIMEDOUT);
  assert(uv_now(&loop) - start < 1500);

  udx_socket_close(&sock);
  udx_socket_close(&sink);
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

  // as if a previous stall left the rto backed off
  stream.rto = 8000;

  uv_update_time(&loop);
  start = uv_now(&loop);

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  uv_buf_t buf = uv_buf_init("hello", 5);
  e = udx_stream_write(req, &stream, &buf, 1, on_ack);
  assert(e);

  // send it, then pretend it went out 12 s ago, so it times out 1 s from now
  uv_run(&loop, UV_RUN_NOWAIT);
  stream.progress_ts -= 12000;

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  return 0;
}

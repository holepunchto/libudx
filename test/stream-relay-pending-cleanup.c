#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

struct sockaddr_storage addr;
int addr_len = sizeof(addr);

udx_socket_t sock;
udx_stream_t astream;
udx_stream_t bstream;
udx_stream_t cstream;
udx_stream_t dstream;

uv_timer_t destroy_timer;
uv_timer_t fail_timer;
udx_stream_write_t *req;

int nclosed = 0;
int nfinalized = 0;
bool socket_closed = false;

void
on_socket_close (udx_socket_t *socket) {
  socket_closed = true;
}

void
on_close (udx_stream_t *stream, int status) {
  nclosed++;

  if (nclosed == 4) {
    uv_timer_stop(&fail_timer);
    uv_close((uv_handle_t *) &fail_timer, NULL);
    udx_socket_close(&sock);
  }
}

void
on_finalize (udx_stream_t *stream) {
  nfinalized++;
}

int
on_c_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  int e = udx_stream_connect(&cstream, socket, dstream.local_id, from);
  assert(e == 0);
  return 0;
}

void
on_destroy_timeout (uv_timer_t *timer) {
  uv_timer_stop(&destroy_timer);
  uv_close((uv_handle_t *) &destroy_timer, NULL);

  udx_stream_destroy(&astream);
  udx_stream_destroy(&bstream);
  udx_stream_destroy(&cstream);
  udx_stream_destroy(&dstream);
}

void
on_fail_timeout (uv_timer_t *timer) {
  assert(false && "pending relay packet cleanup timed out");
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock, on_socket_close);
  assert(e == 0);

  struct sockaddr_in bind_addr;
  uv_ip4_addr("127.0.0.1", 0, &bind_addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &bind_addr, 0);
  assert(e == 0);

  e = udx_socket_getsockname(&sock, (struct sockaddr *) &addr, &addr_len);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, on_close, on_finalize);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, on_close, on_finalize);
  assert(e == 0);

  e = udx_stream_init(&udx, &cstream, 3, on_close, on_finalize);
  assert(e == 0);

  e = udx_stream_init(&udx, &dstream, 4, on_close, on_finalize);
  assert(e == 0);

  e = udx_stream_firewall(&cstream, on_c_firewall);
  assert(e == 0);

  e = udx_stream_relay_to(&cstream, &bstream);
  assert(e == 0);

  e = udx_stream_relay_to(&bstream, &cstream);
  assert(e == 0);

  // d sends before a connects, leaving c->b relay packets pending. The test
  // then destroys every stream to cover queued-packet cleanup/finalization.
  e = udx_stream_connect(&dstream, &sock, cstream.local_id, (struct sockaddr *) &addr);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init("hello", 5);
  e = udx_stream_write(req, &dstream, &buf, 1, NULL);
  assert(e == 1);

  uv_timer_init(&loop, &destroy_timer);
  uv_timer_start(&destroy_timer, on_destroy_timeout, 50, 0);

  uv_timer_init(&loop, &fail_timer);
  uv_timer_start(&fail_timer, on_fail_timeout, 500, 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  assert(nclosed == 4);
  assert(nfinalized == 4);
  assert(socket_closed);

  e = uv_loop_close(&loop);
  assert(e == 0);

  free(req);

  return 0;
}

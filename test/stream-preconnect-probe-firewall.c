#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

struct sockaddr_storage addr;
int addr_len = sizeof(addr);

udx_socket_t sock;
udx_stream_t astream;
udx_stream_t bstream;

uv_timer_t fail_timer;
udx_stream_write_t *req;

int firewall_calls = 0;
int nclosed = 0;
bool read_called = false;

void
on_close (udx_stream_t *stream, int status) {
  nclosed++;

  if (nclosed == 2) {
    uv_timer_stop(&fail_timer);
    uv_close((uv_handle_t *) &fail_timer, NULL);
    udx_socket_close(&sock);
  }
}

void
on_fail_timeout (uv_timer_t *timer) {
  assert(read_called && "preconnect stream did not receive first payload");
}

int
on_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  firewall_calls++;

  // The connect probe should not trigger regular preconnect firewalls.
  assert(firewall_calls == 1);

  return 0;
}

void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  assert(read_len == 5);
  assert(buf->len == 5);
  assert(memcmp(buf->base, "hello", 5) == 0);

  read_called = true;

  int e = udx_stream_connect(&astream, &sock, bstream.local_id, (struct sockaddr *) &addr);
  assert(e == 0);

  udx_stream_destroy(&astream);
  udx_stream_destroy(&bstream);
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock, NULL);
  assert(e == 0);

  struct sockaddr_in bind_addr;
  uv_ip4_addr("127.0.0.1", 0, &bind_addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &bind_addr, 0);
  assert(e == 0);

  e = udx_socket_getsockname(&sock, (struct sockaddr *) &addr, &addr_len);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, on_close, NULL);
  assert(e == 0);

  e = udx_stream_firewall(&astream, on_firewall);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &sock, astream.local_id, (struct sockaddr *) &addr);
  assert(e == 0);

  uv_timer_init(&loop, &fail_timer);
  uv_timer_start(&fail_timer, on_fail_timeout, 500, 0);

  uv_buf_t buf = uv_buf_init("hello", 5);
  e = udx_stream_write(req, &bstream, &buf, 1, NULL);
  assert(e == 1);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  assert(read_called);
  assert(firewall_calls == 1);

  free(req);

  return 0;
}

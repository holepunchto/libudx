#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

#define PAYLOAD_SIZE 8192

uv_loop_t loop;
udx_t udx;

struct sockaddr_storage addr;
int addr_len = sizeof(addr);

udx_socket_t sock;
udx_stream_t astream;
udx_stream_t bstream;
udx_stream_t cstream;
udx_stream_t dstream;

uv_timer_t connect_timer;
uv_timer_t fail_timer;
udx_stream_write_t *req;
char *payload;

bool read_called = false;
size_t nread = 0;
int nclosed = 0;

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
on_fail_timeout (uv_timer_t *timer) {
  assert(read_called && "pending relay packet was not flushed before RTO");
}

void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  assert(read_len > 0);
  assert(buf->len == read_len);
  assert(nread + read_len <= PAYLOAD_SIZE);

  for (ssize_t i = 0; i < read_len; i++) {
    assert(buf->base[i] == payload[nread + i]);
  }

  nread += read_len;

  if (nread == PAYLOAD_SIZE) {
    assert(dstream.rto_count == 0);

    read_called = true;

    udx_stream_destroy(&astream);
    udx_stream_destroy(&bstream);
    udx_stream_destroy(&cstream);
    udx_stream_destroy(&dstream);
  }
}

int
on_b_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  int e = udx_stream_connect(&bstream, socket, astream.local_id, from);
  assert(e == 0);
  return 0;
}

int
on_c_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  int e = udx_stream_connect(&cstream, socket, dstream.local_id, from);
  assert(e == 0);
  return 0;
}

void
on_connect_timeout (uv_timer_t *timer) {
  uv_timer_stop(&connect_timer);
  uv_close((uv_handle_t *) &connect_timer, NULL);

  int e = udx_stream_connect(&astream, &sock, bstream.local_id, (struct sockaddr *) &addr);
  assert(e == 0);
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));
  payload = malloc(PAYLOAD_SIZE);

  for (int i = 0; i < PAYLOAD_SIZE; i++) {
    payload[i] = (char) (i % 251);
  }

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

  // a/d are endpoints; b/c are passive relay streams between them.
  e = udx_stream_init(&udx, &astream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, on_close, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &cstream, 3, on_close, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &dstream, 4, on_close, NULL);
  assert(e == 0);

  e = udx_stream_firewall(&bstream, on_b_firewall);
  assert(e == 0);

  e = udx_stream_firewall(&cstream, on_c_firewall);
  assert(e == 0);

  e = udx_stream_relay_to(&cstream, &bstream);
  assert(e == 0);

  e = udx_stream_relay_to(&bstream, &cstream);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  // d sends before a has connected. The payload spans multiple packets, so
  // this verifies that pending relay packets flush in order before RTO.
  e = udx_stream_connect(&dstream, &sock, cstream.local_id, (struct sockaddr *) &addr);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init(payload, PAYLOAD_SIZE);
  e = udx_stream_write(req, &dstream, &buf, 1, NULL);
  assert(e == 1);

  uv_timer_init(&loop, &connect_timer);
  uv_timer_start(&connect_timer, on_connect_timeout, 50, 0);

  uv_timer_init(&loop, &fail_timer);
  uv_timer_start(&fail_timer, on_fail_timeout, 500, 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  assert(read_called);

  free(req);
  free(payload);

  return 0;
}

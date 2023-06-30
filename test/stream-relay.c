#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

struct sockaddr_in aaddr;
udx_socket_t asock;
udx_stream_t astream;

struct sockaddr_in baddr;
udx_socket_t bsock;
udx_stream_t bstream;

struct sockaddr_in caddr;
udx_socket_t csock;
udx_stream_t cstream;

struct sockaddr_in daddr;
udx_socket_t dsock;
udx_stream_t dstream;

udx_stream_write_t req;

bool ack_called = false;
bool read_called = false;

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  assert(&req == r);
  assert(status == 0);
  assert(unordered == 0);

  uv_stop(&loop);

  ack_called = true;
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  assert(buf->len == 5);
  assert(buf->len == read_len);
  assert(memcmp(buf->base, "hello", 5) == 0);

  read_called = true;
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

  e = udx_socket_init(&udx, &csock);
  assert(e == 0);

  e = udx_socket_init(&udx, &dsock);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 8083, &caddr);
  e = udx_socket_bind(&csock, (struct sockaddr *) &caddr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 8084, &daddr);
  e = udx_socket_bind(&dsock, (struct sockaddr *) &daddr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &cstream, 3, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &dstream, 4, NULL);
  assert(e == 0);

  e = udx_stream_relay_to(&cstream, &bstream);
  assert(e == 0);

  e = udx_stream_relay_to(&bstream, &cstream);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_connect(&cstream, &csock, 4, (struct sockaddr *) &daddr);
  assert(e == 0);

  e = udx_stream_connect(&dstream, &dsock, 3, (struct sockaddr *) &caddr);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init("hello", 5);
  udx_stream_write(&req, &dstream, &buf, 1, on_ack);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(ack_called && read_called);

  return 0;
}

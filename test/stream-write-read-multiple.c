#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_stream_t astream;

udx_socket_t bsock;
udx_stream_t bstream;

int ack_called = 0;
int read_called = 0;

size_t total_read = 0;

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  assert(status == 0);
  assert(unordered == 0);

  if (++ack_called == 2) uv_stop(&loop);
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {

  total_read += read_len;

  assert(memcmp(buf->base, "hello", 5) == 0);

  read_called++;
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &asock, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &bsock, NULL);
  assert(e == 0);

  struct sockaddr_in baddr;
  uv_ip4_addr("127.0.0.1", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr, 0);
  assert(e == 0);

  struct sockaddr_in aaddr;
  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, NULL, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, NULL, NULL);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init("hello", 5);
  printf("starting write\n");

  udx_stream_write_t *areq = malloc(udx_stream_write_sizeof(1));
  e = udx_stream_write(areq, &bstream, &buf, 1, on_ack);
  assert(e && "drained");

  udx_stream_write_t *breq = malloc(udx_stream_write_sizeof(1));
  e = udx_stream_write(breq, &bstream, &buf, 1, on_ack);
  assert(e && "drained");

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(ack_called == 2 && total_read == buf.len * 2);

  return 0;
}

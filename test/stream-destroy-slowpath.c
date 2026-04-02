#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../include/udx.h"

int
close_stream (udx_stream_t *stream, int status);

uv_loop_t loop;
udx_t udx;
udx_socket_t sock;

bool close_called = false;

void
on_close (udx_stream_t *handle, int status) {
  assert(status == 0);

  int e = udx_socket_close(&sock);

  assert(e == 0);

  close_called = true;
}

void
ack_cb (udx_stream_write_t *req, int status, int unordered) {

  printf("ack status=%d unordered=%d\n", status, unordered);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock, NULL);
  assert(e == 0);

  struct sockaddr_in addr;
  uv_ip4_addr("127.0.0.1", 8081, &addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);
  assert(e == 0);

  udx_stream_t stream;
  e = udx_stream_init(&udx, &stream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &addr);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init("hello", 5);
  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

  udx_stream_write(req, &stream, &buf, 1, ack_cb);
  e = udx_stream_destroy(&stream);
  close_stream(&stream, 0);

  uv_run(&loop, UV_RUN_DEFAULT);
  free(req);

  assert(close_called);

  return 0;
}

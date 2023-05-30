#include <assert.h>
#include <stdbool.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;
udx_socket_t sock;

bool close_called = false;

void
on_close (udx_stream_t *handle, int status) {
  assert(status == 0);

  udx_socket_close(&sock, NULL);

  close_called = true;
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock);
  assert(e == 0);

  struct sockaddr_in addr;
  uv_ip4_addr("127.0.0.1", 8081, &addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);
  assert(e == 0);

  udx_stream_t stream;
  e = udx_stream_init(&udx, &stream, 1, on_close);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &addr);
  assert(e == 0);

  e = udx_stream_destroy(&stream);
  assert(e == 1);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(close_called);

  return 0;
}

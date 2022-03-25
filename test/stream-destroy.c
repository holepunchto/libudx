#include <assert.h>
#include <stdbool.h>

#include "../include/udx.h"

uv_loop_t loop;

bool close_called = false;

void
on_close (udx_stream_t *handle, int status) {
  assert(status == 0);

  uv_stop(&loop);

  close_called = true;
}

int
main () {
  int e;

  uv_loop_init(&loop);

  udx_t sock;
  e = udx_init(&loop, &sock);
  assert(e == 0);

  struct sockaddr_in addr;
  uv_ip4_addr("127.0.0.1", 8081, &addr);
  e = udx_bind(&sock, (struct sockaddr *) &addr);
  assert(e == 0);

  udx_stream_t stream;
  e = udx_stream_init(&loop, &stream, 1);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &addr, on_close);
  assert(e == 0);

  e = udx_stream_destroy(&stream);
  assert(e == 1);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(close_called);

  return 0;
}

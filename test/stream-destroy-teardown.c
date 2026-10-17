#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;
udx_socket_t sock;
udx_stream_t stream;
udx_socket_send_t send_req;

int close_calls = 0;

void
on_close (udx_stream_t *handle, int status) {
  close_calls++;
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

  e = udx_stream_init(&udx, &stream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &addr);
  assert(e == 0);

  // queue a send so the destroy packet also takes the slow path
  udx.debug_flags |= UDX_DEBUG_FORCE_SEND_SLOW_PATH;

  uv_buf_t buf = uv_buf_init("hello", 5);
  e = udx_socket_send(&send_req, &sock, &buf, 1, (struct sockaddr *) &addr, NULL);
  assert(e == 0);

  e = udx_stream_destroy(&stream);
  assert(e == 1);
  assert(sock.uv_udp.send_queue_count == 2);

  udx_teardown(&udx);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(close_calls == 1);

  return 0;
}

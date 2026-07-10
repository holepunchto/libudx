#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

/*
 * Regression test for small zero-window writes. Even if queued data is smaller
 * than one packet, it must be sent immediately as a ZWP instead of being
 * deferred as a normal partial packet, with ZWP as the active stream timer.
 */

uv_loop_t loop;
udx_t udx;

udx_socket_t recv_sock;
udx_stream_t recv_stream;

udx_socket_t send_sock;
udx_stream_t send_stream;

udx_stream_write_t *req;

void
bind_addr (struct sockaddr_in *addr, int port) {
  int e = uv_ip4_addr("127.0.0.1", port, addr);
  assert(e == 0);
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  e = uv_loop_init(&loop);
  assert(e == 0);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  struct sockaddr_in recv_addr;
  struct sockaddr_in send_addr;

  bind_addr(&recv_addr, 9091);
  bind_addr(&send_addr, 9092);

  e = udx_socket_init(&udx, &recv_sock, NULL);
  assert(e == 0);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, NULL);
  assert(e == 0);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &recv_stream, 1, NULL, NULL);
  assert(e == 0);
  e = udx_stream_init(&udx, &send_stream, 2, NULL, NULL);
  assert(e == 0);

  send_stream.send_rwnd = 0;

  e = udx_stream_connect(&recv_stream, &recv_sock, 2, (struct sockaddr *) &send_addr);
  assert(e == 0);
  e = udx_stream_connect(&send_stream, &send_sock, 1, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  char data[16];
  memset(data, 0, sizeof(data));
  uv_buf_t buf = uv_buf_init(data, sizeof(data));

  e = udx_stream_write(req, &send_stream, &buf, 1, NULL);
  assert(e && "drained");

  // This write is much smaller than one packet. If ZWP probes are treated like
  // ordinary writes, the partial packet is deferred to the prepare phase and no
  // ZWP timer is armed. A zero-window probe must instead be sent immediately.
  assert(send_stream.seq == 1);
  assert(send_stream.pending_timer == UDX_TIMER_ZWP);
  assert(send_stream.zwp_count == 0);

  free(req);

  return 0;
}

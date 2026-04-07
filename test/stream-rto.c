#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// send a packet into the void and get an RTO

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_stream_t stream;

udx_stream_write_t *req;

bool ack_called = false;

void
on_close (udx_stream_t *s, int status) {
  assert(status == UV_ETIMEDOUT);
  udx_socket_close(&sock);
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == UV_ECANCELED);
  assert(unordered == 0);

  ack_called = true;
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

  struct sockaddr_in baddr;
  uv_ip4_addr("127.0.0.1", 8082, &baddr);

  struct sockaddr_in aaddr;
  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &aaddr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  char *data = malloc(2000); // big enough to take 2 packets so that we exercise free() on stream->pkt and stream->outgoing[]
  assert(data);
  snprintf(data, 2000, "hello");
  uv_buf_t buf = uv_buf_init(data, 2000);

  e = udx_stream_write(req, &stream, &buf, 1, on_ack);

  assert(e);
  // hack to make the packet timeout after 1 RTO, obv. relies
  // on many internal details that are likely to change
  stream.pkt->rto_timeouts = 6;

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  e = uv_loop_close(&loop);
  assert(e == 0);

  free(req);
  free(data);

  assert(ack_called);

  return 0;
}

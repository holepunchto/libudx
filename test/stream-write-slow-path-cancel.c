#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// a write cancelled while queued behind another send still goes out with its bytes

#include "../include/udx.h"

void
close_stream (udx_stream_t *stream, int err);

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_stream_t stream;

udx_socket_send_t send_req;
udx_stream_write_t *req;

char *data;
size_t data_len;

bool ack_called = false;
bool data_received = false;

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == UV_ECANCELED);
  memset(data, 'b', data_len); // the write is done, so the buffer can be reused
  ack_called = true;
}

void
on_recv (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  if (read_len != UDX_HEADER_SIZE + data_len) return;

  for (ssize_t i = UDX_HEADER_SIZE; i < read_len; i++) {
    assert(buf->base[i] == 'a');
  }

  data_received = true;
  udx_socket_close(&sock);
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
  uv_ip4_addr("127.0.0.1", 18181, &addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);
  assert(e == 0);

  e = udx_socket_recv_start(&sock, on_recv); // no stream has id 2, so its packets land here
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, NULL, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &addr);
  assert(e == 0);

  // a send already queued on the socket makes the write take the slow path
  udx.debug_flags |= UDX_DEBUG_FORCE_SEND_SLOW_PATH;
  uv_buf_t x = uv_buf_init("x", 1);
  e = udx_socket_send(&send_req, &sock, &x, 1, (struct sockaddr *) &addr, NULL);
  assert(e == 0);
  udx.debug_flags = 0;

  data_len = stream.pkt_capacity; // a full packet is sent at once
  data = malloc(data_len);
  memset(data, 'a', data_len);
  req = malloc(udx_stream_write_sizeof(1));
  uv_buf_t buf = uv_buf_init(data, data_len);
  e = udx_stream_write(req, &stream, &buf, 1, on_ack);
  assert(e == 1);

  close_stream(&stream, UV_ECONNRESET);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  assert(ack_called);
  assert(data_received);

  free(req);
  free(data);

  return 0;
}

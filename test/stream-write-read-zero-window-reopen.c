#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

/*
 * Regression test for leaving zero-window persist without ACK progress. The
 * initial data-carrying ZWP is dropped, then an unrelated stream message
 * advertises an open window with the same cumulative ACK. RTO must replace ZWP
 * so the outstanding data is eventually retransmitted.
 */

#define DATA_SIZE        16
#define TEST_TIMEOUT_MS  3000

uv_loop_t loop;
udx_t udx;

udx_socket_t recv_sock;
udx_stream_t recv_stream;

udx_socket_t send_sock;
udx_stream_t send_stream;

uv_timer_t timeout;
udx_stream_write_t *write_req;
udx_stream_send_t window_update_req;

bool ack_called;
bool window_update_sent;
size_t bytes_read;

uint32_t
pretend_buffer_is_full (udx_stream_t *stream) {
  return stream->recv_rwnd_max;
}

uint32_t
pretend_buffer_is_empty (udx_stream_t *stream) {
  return 0;
}

void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  assert(read_len == (ssize_t) buf->len);
  bytes_read += read_len;
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  assert(unordered == 0);
  ack_called = true;
  uv_stop(&loop);
}

void
on_window_update_sent (udx_stream_send_t *req, int status) {
  assert(status == 0);
  window_update_sent = true;
}

void
on_timeout (uv_timer_t *timer) {
  uv_stop(timer->loop);
}

void
bind_addr (struct sockaddr_in *addr, int port) {
  int e = uv_ip4_addr("127.0.0.1", port, addr);
  assert(e == 0);
}

int
main () {
  int e;

  write_req = malloc(udx_stream_write_sizeof(1));

  e = uv_loop_init(&loop);
  assert(e == 0);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);
  udx.debug_flags |= UDX_DEBUG_FORCE_DROP_DATA;

  struct sockaddr_in recv_addr;
  struct sockaddr_in send_addr;
  bind_addr(&recv_addr, 9121);
  bind_addr(&send_addr, 9122);

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

  recv_stream.get_read_buffer_size = &pretend_buffer_is_full;
  send_stream.send_rwnd = 0;

  e = udx_stream_connect(&recv_stream, &recv_sock, 2, (struct sockaddr *) &send_addr);
  assert(e == 0);
  e = udx_stream_connect(&send_stream, &send_sock, 1, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_stream_read_start(&recv_stream, on_read);
  assert(e == 0);

  char data[DATA_SIZE];
  memset(data, 0, sizeof(data));
  uv_buf_t data_buf = uv_buf_init(data, sizeof(data));

  e = udx_stream_write(write_req, &send_stream, &data_buf, 1, on_ack);
  assert(e && "drained");

  recv_stream.get_read_buffer_size = &pretend_buffer_is_empty;
  uv_buf_t message = uv_buf_init("", 0);
  e = udx_stream_send(&window_update_req, &recv_stream, &message, 1, on_window_update_sent);
  assert(e == 0);

  e = uv_timer_init(&loop, &timeout);
  assert(e == 0);
  e = uv_timer_start(&timeout, on_timeout, TEST_TIMEOUT_MS, 0);
  assert(e == 0);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(window_update_sent);
  assert(ack_called);
  assert(bytes_read == DATA_SIZE);
  assert(send_stream.send_rwnd > 0);
  assert(send_stream.rto_count > 0);
  assert(send_stream.writes_queued_bytes == 0);

  free(write_req);

  return 0;
}

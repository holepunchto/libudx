#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

/*
 * this test is contrived to start with a receiver advertizing zero receive
 * window to cause the sender to send zero window probes. after two zero-window
 * probes are received we switch to advertizing an empty receive window (see `on_read`
 * below) to allow the data to flow at max speed.
 */

uv_loop_t loop;
udx_t udx;

udx_socket_t recv_sock;
udx_stream_t recv_stream;

udx_socket_t send_sock;
udx_stream_t send_stream;

udx_stream_write_t *req;

int read_counter;
int nprobe_timeouts;
uint64_t start_time_ms;

bool ack_called;

udx_stream_write_t *send_end_req;
udx_stream_write_t *recv_end_req;

int nend;
int nstream_close;
int nsocket_close;
int nfinalize;

void
on_end (udx_stream_write_t *req, int status, int unordered) {
  nend++;
}

void
on_close (udx_stream_t *stream, int status) {
  nstream_close++;
}

void
on_socket_close (udx_socket_t *s) {
  nsocket_close++;
}

void
on_finalize (udx_stream_t *stream) {
  nfinalize++;
  if (nfinalize == 2) {
    udx_socket_close(&send_sock);
    udx_socket_close(&recv_sock);
  }
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  assert(unordered == 0);

  ack_called = true;
  send_end_req = malloc(udx_stream_write_sizeof(1));
  recv_end_req = malloc(udx_stream_write_sizeof(1));

  udx_stream_write_end(send_end_req, &send_stream, NULL, 0, on_end);
  udx_stream_write_end(recv_end_req, &recv_stream, NULL, 0, on_end);
}

uint32_t
pretend_buffer_is_full (udx_stream_t *stream) {
  return stream->recv_rwnd_max;
}

uint32_t
pretend_buffer_is_empty (udx_stream_t *stream) {
  return 0;
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  read_counter++;

  // zwp_count only counts the number of zwp timeouts, since we start with a zero window
  // we automatically probe (without waiting for a timeout) when data is queued.
  // why reac_counter == 2 ? because:
  // read_counter 1: 'free' zwp fired when initially queueing data ona stream with zero window
  // read_counter 2: read fired from the first zero window probe triggered by timeout.

  if (read_counter == 2) {
    handle->get_read_buffer_size = &pretend_buffer_is_empty;
  }
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &recv_sock, on_socket_close);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, on_socket_close);
  assert(e == 0);

  struct sockaddr_in send_addr;
  uv_ip4_addr("127.0.0.1", 8082, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  struct sockaddr_in recv_addr;
  uv_ip4_addr("127.0.0.1", 8081, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &recv_stream, 1, on_close, on_finalize);
  assert(e == 0);

  e = udx_stream_init(&udx, &send_stream, 2, on_close, on_finalize);
  assert(e == 0);

  recv_stream.get_read_buffer_size = &pretend_buffer_is_full;
  send_stream.send_rwnd = 0;
  assert(recv_stream.rto == 1000);

  e = udx_stream_connect(&recv_stream, &recv_sock, 2, (struct sockaddr *) &send_addr);
  assert(e == 0);

  e = udx_stream_connect(&send_stream, &send_sock, 1, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_stream_read_start(&recv_stream, on_read);
  assert(e == 0);

  int data_sz = UDX_MTU_MAX * 6;
  char *data = malloc(data_sz);
  uv_buf_t buf = uv_buf_init(data, data_sz);

  e = udx_stream_write(req, &send_stream, &buf, 1, on_ack);
  assert(e && "drained");

  uv_run(&loop, UV_RUN_DEFAULT);

  // zwp_count only counts the number of zwp timeouts, since we start with a zero window
  // we automatically probe (without waiting for a timeout) when data is queued.

  assert(send_stream.zwp_count == 1 && recv_stream.zwp_count == 0 && ack_called && send_stream.retransmit_count == 0 && recv_stream.retransmit_count == 0);
  assert(nend == 2 && nstream_close == 2 && nfinalize == 2 && nsocket_close == 2);
  free(data);

  return 0;
}

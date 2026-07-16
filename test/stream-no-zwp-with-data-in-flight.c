#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

/*
 * An older data packet is lost before the peer advertises a zero receive
 * window. Queueing another write must not let ZWP displace recovery of that
 * older packet. Once it is retransmitted, the receiver consumes it, opens its
 * window, and both writes can complete.
 */

#define FIRST_DATA_SIZE  (UDX_MTU_BASE - UDX_IPV4_HEADER_SIZE)
#define SECOND_DATA_SIZE 1
#define TEST_TIMEOUT_MS  2000

uv_loop_t loop;
udx_t udx;

udx_socket_t recv_sock;
udx_stream_t recv_stream;

udx_socket_t send_sock;
udx_stream_t send_stream;

udx_stream_write_t *first_req;
udx_stream_write_t *second_req;
udx_stream_send_t window_update_req;

uv_check_t observe_zero_window;
uv_timer_t timeout;

char first_data[FIRST_DATA_SIZE];
char second_data[SECOND_DATA_SIZE];

bool second_write_queued;
int writes_acked;
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

  // Receiving the missing packet lets the application free its buffer. The
  // ACK for this packet will therefore advertise an open receive window.
  recv_stream.get_read_buffer_size = &pretend_buffer_is_empty;
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  writes_acked++;

  if (writes_acked == 2) {
    uv_stop(&loop);
  }
}

void
on_observe_zero_window (uv_check_t *check) {
  if (second_write_queued || send_stream.send_rwnd != 0) {
    return;
  }

  second_write_queued = true;
  uv_check_stop(check);

  uv_buf_t buf = uv_buf_init(second_data, sizeof(second_data));
  int e = udx_stream_write(
    second_req,
    &send_stream,
    &buf,
    1,
    on_ack
  );
  assert(e && "drained");
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

  first_req = malloc(udx_stream_write_sizeof(1));
  second_req = malloc(udx_stream_write_sizeof(1));

  e = uv_loop_init(&loop);
  assert(e == 0);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  struct sockaddr_in recv_addr;
  struct sockaddr_in send_addr;
  bind_addr(&recv_addr, 9131);
  bind_addr(&send_addr, 9132);

  e = udx_socket_init(&udx, &recv_sock, NULL);
  assert(e == 0);
  e = udx_socket_bind(
    &recv_sock,
    (struct sockaddr *) &recv_addr,
    0
  );
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, NULL);
  assert(e == 0);
  e = udx_socket_bind(
    &send_sock,
    (struct sockaddr *) &send_addr,
    0
  );
  assert(e == 0);

  e = udx_stream_init(&udx, &recv_stream, 1, NULL, NULL);
  assert(e == 0);
  e = udx_stream_init(&udx, &send_stream, 2, NULL, NULL);
  assert(e == 0);

  recv_stream.get_read_buffer_size = &pretend_buffer_is_full;
  send_stream.send_rwnd = FIRST_DATA_SIZE;
  send_stream.rto = 200;

  e = udx_stream_connect(
    &recv_stream,
    &recv_sock,
    2,
    (struct sockaddr *) &send_addr
  );
  assert(e == 0);

  e = udx_stream_connect(
    &send_stream,
    &send_sock,
    1,
    (struct sockaddr *) &recv_addr
  );
  assert(e == 0);

  e = udx_stream_read_start(&recv_stream, on_read);
  assert(e == 0);

  memset(first_data, 0, sizeof(first_data));
  memset(second_data, 0, sizeof(second_data));

  // Send one full packet and drop only its first transmission.
  udx.debug_flags |= UDX_DEBUG_FORCE_DROP_DATA;

  uv_buf_t first_buf = uv_buf_init(
    first_data,
    sizeof(first_data)
  );

  e = udx_stream_write(
    first_req,
    &send_stream,
    &first_buf,
    1,
    on_ack
  );
  assert(e && "drained");

  udx.debug_flags &= ~UDX_DEBUG_FORCE_DROP_DATA;

  assert(send_stream.remote_acked != send_stream.seq);
  assert(
    send_stream.pending_timer == UDX_TIMER_RTO ||
    send_stream.pending_timer == UDX_TIMER_TLP
  );

  // Advertise zero window without advancing the cumulative ACK.
  uv_buf_t update = uv_buf_init("", 0);

  e = udx_stream_send(
    &window_update_req,
    &recv_stream,
    &update,
    1,
    NULL
  );
  assert(e == 0);

  // Queue the second write only after the sender observes zero window.
  e = uv_check_init(&loop, &observe_zero_window);
  assert(e == 0);

  e = uv_check_start(
    &observe_zero_window,
    on_observe_zero_window
  );
  assert(e == 0);

  e = uv_timer_init(&loop, &timeout);
  assert(e == 0);

  e = uv_timer_start(
    &timeout,
    on_timeout,
    TEST_TIMEOUT_MS,
    0
  );
  assert(e == 0);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(second_write_queued);

  assert(
    send_stream.retransmit_count > 0 &&
    "zero-window state displaced loss recovery"
  );

  assert(
    bytes_read == FIRST_DATA_SIZE + SECOND_DATA_SIZE
  );

  assert(writes_acked == 2);

  free(first_req);
  free(second_req);

  return 0;
}

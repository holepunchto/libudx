#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

#include "../include/udx.h"

#define PACKET_COUNT    10
#define PACKET_PAYLOAD  (UDX_MTU_BASE - UDX_IPV4_HEADER_SIZE)
#define TEST_TIMEOUT_MS 5000

uv_loop_t loop;
udx_t udx;

udx_socket_t send_socket;
udx_socket_t sink_socket;
udx_stream_t stream;

udx_stream_write_t *write_req;
uv_check_t observe_rto;
uv_timer_t timeout;

uint16_t first_rto_retransmits;

static void
on_sink_recv (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) socket;
  (void) read_len;
  (void) buf;
  (void) from;

  // Intentionally do not ACK. This leaves a complete congestion window in
  // flight so the first RTO exercises BBR's packet-conservation behavior.
}

static void
on_write_ack (udx_stream_write_t *req, int status, int unordered) {
  (void) req;

  assert(status == UV_ECANCELED);
  assert(unordered == 0);
}

static void
on_stream_close (udx_stream_t *closed_stream, int status) {
  (void) closed_stream;

  assert(status == 0);
  assert(udx_socket_close(&send_socket) == 0);
  assert(udx_socket_close(&sink_socket) == 0);
}

static void
on_observe_rto (uv_check_t *check) {
  if (stream.rto_count == 0) return;

  assert(stream.rto_count == 1);
  assert(stream.seq == PACKET_COUNT);
  assert(stream.bbr.prior_cwnd == PACKET_COUNT);
  assert(stream.cwnd == 1);

  first_rto_retransmits = stream.retransmit_count;

  // One RTO should permit one retransmission, rather than replaying the
  // complete congestion window as a burst.
  assert(first_rto_retransmits == 1);

  uv_check_stop(check);
  uv_close((uv_handle_t *) check, NULL);
  uv_timer_stop(&timeout);
  uv_close((uv_handle_t *) &timeout, NULL);

  assert(udx_stream_destroy(&stream) >= 0);
}

static void
on_timeout (uv_timer_t *timer) {
  (void) timer;
  assert(false && "first RTO was not observed");
}

int
main () {
  int err;

  write_req = malloc(udx_stream_write_sizeof(1));
  assert(write_req != NULL);

  char *data = calloc(PACKET_COUNT, PACKET_PAYLOAD);
  assert(data != NULL);

  err = uv_loop_init(&loop);
  assert(err == 0);

  err = udx_init(&loop, &udx, NULL);
  assert(err == 0);

  err = udx_socket_init(&udx, &send_socket, NULL);
  assert(err == 0);

  err = udx_socket_init(&udx, &sink_socket, NULL);
  assert(err == 0);

  struct sockaddr_in send_addr;
  err = uv_ip4_addr("127.0.0.1", 9181, &send_addr);
  assert(err == 0);
  err = udx_socket_bind(&send_socket, (struct sockaddr *) &send_addr, 0);
  assert(err == 0);

  struct sockaddr_in sink_addr;
  err = uv_ip4_addr("127.0.0.1", 9182, &sink_addr);
  assert(err == 0);
  err = udx_socket_bind(&sink_socket, (struct sockaddr *) &sink_addr, 0);
  assert(err == 0);

  err = udx_socket_recv_start(&sink_socket, on_sink_recv);
  assert(err == 0);

  err = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(err == 0);

  err = udx_stream_connect(&stream, &send_socket, 2, (struct sockaddr *) &sink_addr);
  assert(err == 0);

  stream.rto = 100;

  uv_buf_t buf = uv_buf_init(data, PACKET_COUNT * PACKET_PAYLOAD);
  err = udx_stream_write(write_req, &stream, &buf, 1, on_write_ack);
  assert(err && "drained");

  err = uv_check_init(&loop, &observe_rto);
  assert(err == 0);
  err = uv_check_start(&observe_rto, on_observe_rto);
  assert(err == 0);

  err = uv_timer_init(&loop, &timeout);
  assert(err == 0);
  err = uv_timer_start(&timeout, on_timeout, TEST_TIMEOUT_MS, 0);
  assert(err == 0);

  err = uv_run(&loop, UV_RUN_DEFAULT);
  assert(err == 0);

  err = uv_loop_close(&loop);
  assert(err == 0);

  assert(first_rto_retransmits == 1);

  free(write_req);
  free(data);

  return 0;
}

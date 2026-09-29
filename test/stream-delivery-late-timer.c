// Keep the checks active in Release builds too.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

// A blocked event loop (or a suspended process) must not be charged to the
// delivery budget: block the loop for longer than the budget while data is
// outstanding, and check that the streams do not close with UV_ETIMEDOUT and
// that the data is delivered once the peers are reachable.
//
// pair A: RTT known (srtt 5 ms), written right before the stall, so the timer
//         that fires late is the tail loss probe. Its receiver appears only
//         shortly after the stall, so the data sent before the stall and the
//         late probe are dropped: delivery needs the RTO retransmission one
//         RTO after the loop resumed, which must not be cut short.
// pair B: no RTT, written at start while its receiver does not exist yet; it
//         has retransmitted once, so the timer that fires late is the RTO. Its
//         receiver appears right before the stall, so the retransmission sent
//         when the loop resumes is delivered.

#define TIMEOUT_MS      3000 // explicit delivery budget, 3 * 1000 ms base RTO
#define STALL_AT_MS     1500
#define STALL_MS        (TIMEOUT_MS + 1500)
#define RECEIVERS_AFTER 20 // ms after the stall
#define PAYLOAD         4000

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

udx_stream_t a_send;
udx_stream_t a_recv;
udx_stream_t b_send;
udx_stream_t b_recv;

udx_stream_write_t *a_req;
udx_stream_write_t *b_req;

uv_timer_t stall_timer;
uv_timer_t receivers_timer; // starts pair A's receiver
uv_timer_t watchdog;

char a_payload[PAYLOAD];
char b_payload[PAYLOAD];

size_t a_read;
size_t b_read;
int a_ack_status = 1;
int b_ack_status = 1;
int open_streams = 2;
int close_statuses[4] = {1, 1, 1, 1};
uint64_t start_ns;
uint64_t stall_end_ms;
uint64_t a_acked_ms;
uint64_t b_acked_ms;

static uint64_t
elapsed_ms () {
  return (uv_hrtime() - start_ns) / 1000000;
}

static void
on_close (udx_stream_t *stream, int status) {
  int idx = (int) (intptr_t) stream->data;
  close_statuses[idx] = status;
  fprintf(stderr, "stream %d closed with %d at %llu ms\n", idx, status, (unsigned long long) elapsed_ms());
  assert(status != UV_ETIMEDOUT);

  if (--open_streams == 0) {
    udx_socket_close(&send_sock);
    udx_socket_close(&recv_sock);
  }
}

static void
maybe_finish () {
  if (a_ack_status != 0 || b_ack_status != 0) return;
  if (a_read != PAYLOAD || b_read != PAYLOAD) return;

  uv_close((uv_handle_t *) &watchdog, NULL);

  udx_stream_destroy(&a_send);
  udx_stream_destroy(&a_recv);
  udx_stream_destroy(&b_send);
  udx_stream_destroy(&b_recv);
}

static void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  (void) buf;
  assert(read_len > 0);
  if (stream == &a_recv) {
    a_read += read_len;
  } else {
    assert(stream == &b_recv);
    b_read += read_len;
  }
  maybe_finish();
}

static void
on_payload_ack (udx_stream_write_t *req, int status, int unordered) {
  (void) unordered;
  if (req == a_req) {
    a_ack_status = status;
    a_acked_ms = elapsed_ms();
  } else {
    b_ack_status = status;
    b_acked_ms = elapsed_ms();
  }
  assert(status == 0);
  maybe_finish();
}

static void
start_receiver (udx_stream_t *stream, uint32_t local_id, uint32_t remote_id, int idx) {
  int e = udx_stream_init(&udx, stream, local_id, on_close, NULL);
  assert(e == 0);
  stream->data = (void *) (intptr_t) idx;
  open_streams++;
  e = udx_stream_connect(stream, &recv_sock, remote_id, (struct sockaddr *) &send_addr);
  assert(e == 0);
  e = udx_stream_read_start(stream, on_read);
  assert(e == 0);
}

static void
on_receivers (uv_timer_t *timer) {
  (void) timer;
  uv_close((uv_handle_t *) &receivers_timer, NULL);

  fprintf(stderr, "receiver a at %llu ms: a lifetime_rto_count=%u retransmit_count=%u, b lifetime_rto_count=%u retransmit_count=%u\n", (unsigned long long) elapsed_ms(), a_send.lifetime_rto_count, a_send.retransmit_count, b_send.lifetime_rto_count, b_send.retransmit_count);

  start_receiver(&a_recv, 2, 1, 1);
}

static void
on_stall (uv_timer_t *timer) {
  (void) timer;
  int e;

  assert(b_send.remote_acked != b_send.seq);
  assert(b_send.lifetime_rto_count >= 1);

  uv_close((uv_handle_t *) &stall_timer, NULL);

  // pair A: write, then stall with the tail loss probe pending
  uv_buf_t buf = uv_buf_init(a_payload, PAYLOAD);
  e = udx_stream_write(a_req, &a_send, &buf, 1, on_payload_ack);
  assert(e >= 0);

  // pair B: the receiver appears, the sender's RTO is pending
  start_receiver(&b_recv, 4, 3, 3);

  // pair A: the receiver appears shortly after the stall (loop time is not updated
  // while this callback runs, so this fires RECEIVERS_AFTER ms after it)
  e = uv_timer_start(&receivers_timer, on_receivers, STALL_MS + RECEIVERS_AFTER, 0);
  assert(e == 0);

  fprintf(stderr, "stall at %llu ms: a pending_timer=%d, b pending_timer=%d b lifetime_rto_count=%u\n", (unsigned long long) elapsed_ms(), a_send.pending_timer, b_send.pending_timer, b_send.lifetime_rto_count);

  uv_sleep(STALL_MS);

  stall_end_ms = elapsed_ms();
}

static void
on_watchdog (uv_timer_t *timer) {
  (void) timer;
  fprintf(stderr, "late-timer test timed out: a_read=%zu b_read=%zu a_ack=%d b_ack=%d\n", a_read, b_read, a_ack_status, b_ack_status);
  abort();
}

static void
bind_any (udx_socket_t *sock, struct sockaddr_in *addr) {
  int e = udx_socket_init(&udx, sock, NULL);
  assert(e == 0);
  uv_ip4_addr("127.0.0.1", 0, addr);
  e = udx_socket_bind(sock, (struct sockaddr *) addr, 0);
  assert(e == 0);
  int len = sizeof(*addr);
  e = udx_socket_getsockname(sock, (struct sockaddr *) addr, &len);
  assert(e == 0);
}

int
main () {
  int e;

  memset(a_payload, 'a', PAYLOAD);
  memset(b_payload, 'b', PAYLOAD);

  e = uv_loop_init(&loop);
  assert(e == 0);
  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  bind_any(&send_sock, &send_addr);
  bind_any(&recv_sock, &recv_addr);

  e = udx_stream_init(&udx, &a_send, 1, on_close, NULL);
  assert(e == 0);
  a_send.data = (void *) (intptr_t) 0;
  e = udx_stream_init(&udx, &b_send, 3, on_close, NULL);
  assert(e == 0);
  b_send.data = (void *) (intptr_t) 2;

  e = udx_stream_connect(&a_send, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);
  e = udx_stream_connect(&b_send, &send_sock, 4, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  // pair A: pretend an RTT estimate exists so that a tail loss probe is armed
  a_send.srtt = 5;
  a_send.rttvar = 2;
  a_send.rto = 1000;
  a_send.tlp_permitted = true;

  e = udx_stream_set_delivery_timeout(&a_send, TIMEOUT_MS);
  assert(e == 0);
  e = udx_stream_set_delivery_timeout(&b_send, TIMEOUT_MS);
  assert(e == 0);

  a_req = malloc(udx_stream_write_sizeof(1));
  b_req = malloc(udx_stream_write_sizeof(1));

  e = uv_timer_init(&loop, &stall_timer);
  assert(e == 0);
  e = uv_timer_init(&loop, &receivers_timer);
  assert(e == 0);
  e = uv_timer_init(&loop, &watchdog);
  assert(e == 0);

  uv_update_time(&loop);
  start_ns = uv_hrtime();

  // pair B: nobody listens on stream id 4 yet, the packets are dropped
  uv_buf_t buf = uv_buf_init(b_payload, PAYLOAD);
  e = udx_stream_write(b_req, &b_send, &buf, 1, on_payload_ack);
  assert(e >= 0);

  e = uv_timer_start(&stall_timer, on_stall, STALL_AT_MS, 0);
  assert(e == 0);
  e = uv_timer_start(&watchdog, on_watchdog, STALL_AT_MS + STALL_MS + 10000, 0);
  assert(e == 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  e = uv_loop_close(&loop);
  assert(e == 0);

  fprintf(stderr, "stall ended at %llu ms, a acked at %llu ms, b acked at %llu ms\n", (unsigned long long) stall_end_ms, (unsigned long long) a_acked_ms, (unsigned long long) b_acked_ms);

  assert(stall_end_ms >= STALL_AT_MS + STALL_MS);
  assert(a_ack_status == 0 && b_ack_status == 0);
  assert(a_read == PAYLOAD && b_read == PAYLOAD);
  for (int i = 0; i < 4; i++) assert(close_statuses[i] == 0);

  free(a_req);
  free(b_req);
  return 0;
}

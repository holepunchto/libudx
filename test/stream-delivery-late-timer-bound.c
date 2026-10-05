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

// Late retransmission timer firings are not charged to the delivery budget,
// but only UDX_MAX_RTO_TIMEOUTS of them per cumulative ack advance: a loop that
// blocks for more than two RTOs on every iteration makes every firing late, and
// the stream must still close with UV_ETIMEDOUT when the peer is dead.
//
// The sink never acks. With a 3000 ms delivery budget and a 1000 ms RTO every
// timer firing is late, the first MAX_LATE_FIRINGS (TLP or RTO) are absorbed
// and the next one closes the stream, after MAX_LATE_FIRINGS + 2 stalls.
// Without the bound it never closes.

#define MAX_LATE_FIRINGS 6 // UDX_MAX_RTO_TIMEOUTS in src/udx.c
#define TIMEOUT_MS       3000
#define LAG_MS           2300                   // more than 2 * 1000 ms RTO
#define MAX_STALLS       (MAX_LATE_FIRINGS + 3) // one stall of slack

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_socket_t sink;
struct sockaddr_in sink_addr;

udx_stream_t stream;
udx_stream_write_t *req;
char payload[64];

uv_timer_t lag_timer;

uint64_t start_ns;
uint64_t closed_ms;
int close_status = 1;
int ack_status = 1;
int lags;

static uint64_t
elapsed_ms () {
  return (uv_hrtime() - start_ns) / 1000000;
}

static void
on_sink_read (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) socket;
  (void) read_len;
  (void) buf;
  (void) from;
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  (void) req;
  (void) unordered;
  ack_status = status;
}

static void
on_close (udx_stream_t *stream, int status) {
  closed_ms = elapsed_ms();
  close_status = status;

  fprintf(stderr, "closed %d at %llu ms after %d stalls, late_timer_count=%u lifetime_rto_count=%u retransmit_count=%u\n", status, (unsigned long long) closed_ms, lags, stream->late_timer_count, stream->lifetime_rto_count, stream->retransmit_count);

  assert(stream->late_timer_count == MAX_LATE_FIRINGS);

  uv_close((uv_handle_t *) &lag_timer, NULL);
  udx_socket_close(&sock);
  udx_socket_close(&sink);
}

static void
on_lag (uv_timer_t *timer) {
  (void) timer;

  if (lags == MAX_STALLS) {
    fprintf(stderr, "not closed at %llu ms after %d stalls, late_timer_count=%u lifetime_rto_count=%u retransmit_count=%u\n", (unsigned long long) elapsed_ms(), lags, stream.late_timer_count, stream.lifetime_rto_count, stream.retransmit_count);
    abort();
  }

  lags++;
  uv_sleep(LAG_MS);
}

static void
bind_any (udx_socket_t *socket, struct sockaddr_in *addr) {
  int e = udx_socket_init(&udx, socket, NULL);
  assert(e == 0);
  uv_ip4_addr("127.0.0.1", 0, addr);
  e = udx_socket_bind(socket, (struct sockaddr *) addr, 0);
  assert(e == 0);
  int len = sizeof(*addr);
  e = udx_socket_getsockname(socket, (struct sockaddr *) addr, &len);
  assert(e == 0);
}

int
main () {
  int e;

  e = uv_loop_init(&loop);
  assert(e == 0);
  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  struct sockaddr_in addr;
  bind_any(&sock, &addr);
  bind_any(&sink, &sink_addr);
  e = udx_socket_recv_start(&sink, on_sink_read);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_close, NULL);
  assert(e == 0);
  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &sink_addr);
  assert(e == 0);
  e = udx_stream_set_delivery_timeout(&stream, TIMEOUT_MS);
  assert(e == 0);

  e = uv_timer_init(&loop, &lag_timer);
  assert(e == 0);

  memset(payload, 'a', sizeof(payload));
  req = malloc(udx_stream_write_sizeof(1));

  uv_update_time(&loop);
  start_ns = uv_hrtime();

  uv_buf_t buf = uv_buf_init(payload, sizeof(payload));
  e = udx_stream_write(req, &stream, &buf, 1, on_ack);
  assert(e >= 0);

  // block the loop on every iteration
  e = uv_timer_start(&lag_timer, on_lag, 1, 1);
  assert(e == 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  e = uv_loop_close(&loop);
  assert(e == 0);

  assert(close_status == UV_ETIMEDOUT);
  assert(ack_status == UV_ECANCELED);
  // the late firings were absorbed, so the close comes long after the budget
  assert(closed_ms >= MAX_LATE_FIRINGS * LAG_MS);

  free(req);
  return 0;
}

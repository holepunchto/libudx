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

// Change the delivery timeout of a stream that is already stalled, into a sink
// that never acks. No RTT estimate, so RTOs fire at 1000, 3000, 5000, ... ms.
// The new timeout counts from the first send and applies to the RTO that is
// already pending: shortening it must not wait for that RTO.

#define TOLERANCE_MS 300

typedef struct {
  const char *name;
  uint32_t initial_ms; // 0: default budget
  uint64_t set_at_ms;
  uint32_t set_ms;
  uint64_t expected_ms;

  udx_socket_t sock;
  udx_stream_t stream;
  udx_stream_write_t *req;
  uv_timer_t set_timer;
  uint64_t closed_ms;
  int close_status;
  int ack_status;
} test_case_t;

static test_case_t cases[] = {
  // default budget (13000 ms), at 3500 ms set 4000: the RTO pending for 5000 is pulled in to 4000
  {.name = "shorten", .set_at_ms = 3500, .set_ms = 4000, .expected_ms = 4000},
  // default budget, at 3500 ms set 3000: the deadline has passed, close at the next firing, now
  {.name = "shorten-past", .set_at_ms = 3500, .set_ms = 3000, .expected_ms = 3500},
  // explicit 3000 ms, at 2500 ms set 8000: the RTO pending for 3000 does not close, close at 8000
  {.name = "extend", .initial_ms = 3000, .set_at_ms = 2500, .set_ms = 8000, .expected_ms = 8000},
};

#define NCASES (sizeof(cases) / sizeof(cases[0]))

uv_loop_t loop;
udx_t udx;
udx_socket_t sink;
struct sockaddr_in sink_addr;
char payload[64];
int open_streams = NCASES;
uint64_t start_ns;

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
  (void) unordered;
  test_case_t *c = req->data;
  c->ack_status = status;
}

static void
on_close (udx_stream_t *stream, int status) {
  test_case_t *c = stream->data;
  c->closed_ms = elapsed_ms();
  c->close_status = status;
  udx_socket_close(&c->sock);

  if (--open_streams == 0) udx_socket_close(&sink);
}

static void
on_set (uv_timer_t *timer) {
  test_case_t *c = timer->data;
  uv_close((uv_handle_t *) timer, NULL);

  assert(c->close_status == 1);
  assert(c->stream.lifetime_rto_count >= 1);

  int e = udx_stream_set_delivery_timeout(&c->stream, c->set_ms);
  assert(e == 0);
}

int
main () {
  int e;

  e = uv_loop_init(&loop);
  assert(e == 0);
  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &sink, NULL);
  assert(e == 0);
  uv_ip4_addr("127.0.0.1", 0, &sink_addr);
  e = udx_socket_bind(&sink, (struct sockaddr *) &sink_addr, 0);
  assert(e == 0);
  int len = sizeof(sink_addr);
  e = udx_socket_getsockname(&sink, (struct sockaddr *) &sink_addr, &len);
  assert(e == 0);
  e = udx_socket_recv_start(&sink, on_sink_read);
  assert(e == 0);

  memset(payload, 'a', sizeof(payload));
  uv_update_time(&loop);
  start_ns = uv_hrtime();

  for (size_t i = 0; i < NCASES; i++) {
    test_case_t *c = &cases[i];
    c->close_status = 1;
    c->ack_status = 1;

    e = udx_socket_init(&udx, &c->sock, NULL);
    assert(e == 0);
    struct sockaddr_in addr;
    uv_ip4_addr("127.0.0.1", 0, &addr);
    e = udx_socket_bind(&c->sock, (struct sockaddr *) &addr, 0);
    assert(e == 0);

    e = udx_stream_init(&udx, &c->stream, 1 + i, on_close, NULL);
    assert(e == 0);
    c->stream.data = c;
    e = udx_stream_connect(&c->stream, &c->sock, 100 + i, (struct sockaddr *) &sink_addr);
    assert(e == 0);
    e = udx_stream_set_delivery_timeout(&c->stream, c->initial_ms);
    assert(e == 0);

    c->req = malloc(udx_stream_write_sizeof(1));
    c->req->data = c;
    uv_buf_t buf = uv_buf_init(payload, sizeof(payload));
    e = udx_stream_write(c->req, &c->stream, &buf, 1, on_ack);
    assert(e >= 0);

    e = uv_timer_init(&loop, &c->set_timer);
    assert(e == 0);
    c->set_timer.data = c;
    e = uv_timer_start(&c->set_timer, on_set, c->set_at_ms, 0);
    assert(e == 0);
  }

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  e = uv_loop_close(&loop);
  assert(e == 0);

  bool ok = true;

  for (size_t i = 0; i < NCASES; i++) {
    test_case_t *c = &cases[i];
    fprintf(stderr, "%s: closed %d at %llu ms (expected %llu, max +%d), lifetime_rto_count=%u retransmit_count=%u\n", c->name, c->close_status, (unsigned long long) c->closed_ms, (unsigned long long) c->expected_ms, TOLERANCE_MS, c->stream.lifetime_rto_count, c->stream.retransmit_count);

    if (c->close_status != UV_ETIMEDOUT) ok = false;
    if (c->ack_status != UV_ECANCELED) ok = false;
    // never early (2 ms for millisecond truncation of loop time), at most TOLERANCE_MS late
    if (c->closed_ms + 2 < c->expected_ms || c->closed_ms > c->expected_ms + TOLERANCE_MS) ok = false;
    free(c->req);
  }

  assert(ok);
  return 0;
}

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
#include "../src/endian.h"

// Send into a sink that never acks with an explicit delivery timeout and check
// that the stream closes with UV_ETIMEDOUT at that timeout, after at least two
// retransmissions, and that values below three RTOs are raised to three RTOs.

#define TOLERANCE_MS 300

typedef struct {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t data_offset;
  uint32_t id;
  uint32_t rwnd;
  uint32_t seq;
  uint32_t ack;
} packet_header_t;

typedef struct {
  const char *name;
  uint32_t srtt;
  uint32_t rttvar;
  uint32_t rto;
  uint32_t timeout_ms;
  uint64_t expected_ms;

  udx_socket_t sock;
  udx_stream_t stream;
  udx_stream_write_t *req;
  uint64_t start_ns;
  uint64_t closed_ms;
  int close_status;
  int ack_status;
  int transmits;
  uint64_t transmit_ms[32];
} test_case_t;

static test_case_t cases[] = {
  // explicit 3000 ms, no RTT estimate (rto 1000): RTOs at 1000 and 2000, close at 3000
  {.name = "explicit-3000", .timeout_ms = 3000, .rto = 1000, .expected_ms = 3000},
  // 100 ms is below 3 * 1000 ms and is raised to 3000 ms
  {.name = "explicit-100-floored", .timeout_ms = 100, .rto = 1000, .expected_ms = 3000},
};

#define NCASES (sizeof(cases) / sizeof(cases[0]))

uv_loop_t loop;
udx_t udx;
udx_socket_t sink;
struct sockaddr_in sink_addr;
char payload[64];
int open_streams = NCASES;

static void
on_sink_read (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) socket;
  (void) from;
  if (read_len < (ssize_t) sizeof(packet_header_t)) return;

  packet_header_t *header = (packet_header_t *) buf->base;
  if (!(header->type & UDX_HEADER_DATA)) return;

  uint32_t id = udx__swap_uint32_if_be(header->id);
  assert(id >= 100 && id < 100 + NCASES);
  test_case_t *c = &cases[id - 100];
  if (c->transmits < 32) c->transmit_ms[c->transmits] = (uv_hrtime() - c->start_ns) / 1000000;
  c->transmits++;
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
  c->closed_ms = (uv_hrtime() - c->start_ns) / 1000000;
  c->close_status = status;
  udx_socket_close(&c->sock);

  if (--open_streams == 0) udx_socket_close(&sink);
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

  for (size_t i = 0; i < NCASES; i++) {
    test_case_t *c = &cases[i];

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

    // pretend an RTT estimate exists, as for a stream that has been in use
    c->stream.srtt = c->srtt;
    c->stream.rttvar = c->rttvar;
    c->stream.rto = c->rto;
    c->stream.tlp_permitted = c->srtt > 0;
    e = udx_stream_set_delivery_timeout(&c->stream, c->timeout_ms);
    assert(e == 0);

    c->req = malloc(udx_stream_write_sizeof(1));
    c->req->data = c;
    uv_buf_t buf = uv_buf_init(payload, sizeof(payload));
    c->start_ns = uv_hrtime();
    e = udx_stream_write(c->req, &c->stream, &buf, 1, on_ack);
    assert(e >= 0);
  }

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  e = uv_loop_close(&loop);
  assert(e == 0);

  bool ok = true;

  for (size_t i = 0; i < NCASES; i++) {
    test_case_t *c = &cases[i];
    fprintf(stderr, "%s: closed %d at %llu ms (expected %llu, max +%d), rto_count=%u lifetime_rto_count=%u, retransmit_count=%u, %d transmissions at", c->name, c->close_status, (unsigned long long) c->closed_ms, (unsigned long long) c->expected_ms, TOLERANCE_MS, c->stream.rto_count, c->stream.lifetime_rto_count, c->stream.retransmit_count, c->transmits);
    for (int j = 0; j < c->transmits && j < 32; j++) fprintf(stderr, " %llu", (unsigned long long) c->transmit_ms[j]);
    fprintf(stderr, "\n");

    if (c->close_status != UV_ETIMEDOUT) ok = false;
    if (c->ack_status != UV_ECANCELED) ok = false;
    // never early (2 ms for millisecond truncation of loop time), at most TOLERANCE_MS late
    if (c->closed_ms + 2 < c->expected_ms || c->closed_ms > c->expected_ms + TOLERANCE_MS) ok = false;
    // the original transmission plus at least two retransmissions
    if (c->transmits < 3 || c->stream.retransmit_count < 2) ok = false;
    free(c->req);
  }

  assert(ok);
  return 0;
}

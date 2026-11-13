#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/cirbuf.h"
#include "../src/endian.h"
#include "../src/internal.h"

// The peer holds the acks for a burst and sends them back to back, as if they had
// queued while our loop was stalled for 15 s. Every one of them gives a clamped
// sample, but they all measure the same stall: only the first may feed the estimator.

#define BURST    8
#define STALL_MS 15000

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_stream_t stream;

uv_udp_t peer;
uv_timer_t ack_timer;
uv_timer_t poll_timer;

struct sockaddr_in sock_addr;
struct sockaddr_in peer_addr;

uint32_t seen[BURST + 1];
uint32_t received;
uint32_t srtt;
uint32_t rttvar;
uint64_t started;
int phase;

struct {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t data_offset;
  uint32_t remote_id;
  uint32_t rwnd;
  uint32_t seq;
  uint32_t ack;
} ack_packet;

static void
send_ack (uint32_t ack) {
  ack_packet.ack = udx__swap_uint32_if_be(ack);

  uv_buf_t buf = uv_buf_init((char *) &ack_packet, sizeof(ack_packet));
  int e = uv_udp_try_send(&peer, &buf, 1, (struct sockaddr *) &sock_addr);
  assert(e == sizeof(ack_packet));
}

static void
on_ack_timer (uv_timer_t *timer) {
  send_ack(1);
}

static void
on_alloc (uv_handle_t *handle, size_t size, uv_buf_t *buf) {
  static char data[2048];
  *buf = uv_buf_init(data, sizeof(data));
}

static void
on_recv (uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags) {
  if (nread < 20 || !(buf->base[2] & UDX_HEADER_DATA)) return;

  uint32_t seq;
  memcpy(&seq, buf->base + 12, 4);
  seq = udx__swap_uint32_if_be(seq);
  assert(seq <= BURST);

  if (seen[seq]++) return;

  if (seq == 0) {
    uv_timer_start(&ack_timer, on_ack_timer, 100, 0);
    return;
  }

  // as if sent before the stall: move the packet, and rack's newest send time once, back by it
  udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&stream.outgoing, seq);
  assert(pkt != NULL);
  pkt->time_sent -= STALL_MS;
  if (++received == 1) stream.rack_time_sent -= STALL_MS;
  if (received < BURST) return;

  srtt = stream.srtt;
  rttvar = stream.rttvar;

  for (uint32_t ack = 2; ack <= BURST + 1; ack++) send_ack(ack);
}

static void
write_bytes (size_t len) {
  char *data = malloc(len);
  assert(data != NULL);
  memset(data, 'a', len);

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  assert(req != NULL);

  uv_buf_t buf = uv_buf_init(data, len);
  udx_stream_write(req, &stream, &buf, 1, NULL);
}

static void
on_poll (uv_timer_t *timer) {
  assert(uv_now(&loop) - started < 3000);

  if (phase == 0 && stream.remote_acked == 1) {
    assert(stream.rttvar > 0);
    phase = 1;
    write_bytes(udx__max_payload(&stream) * BURST);
  } else if (phase == 1 && stream.remote_acked == BURST + 1) {
    // one clamped sample (srtt + 5 * rttvar) doubles rttvar, each further one doubles it again
    assert(stream.srtt == (8 * srtt + 5 * rttvar) / 8);
    assert(stream.rttvar == 2 * rttvar);

    uv_stop(&loop);
  }
}

int
main () {
  int e;

  e = uv_loop_init(&loop);
  assert(e == 0);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18141, &sock_addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &sock_addr, 0);
  assert(e == 0);

  e = uv_udp_init(&loop, &peer);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18142, &peer_addr);
  e = uv_udp_bind(&peer, (struct sockaddr *) &peer_addr, 0);
  assert(e == 0);

  e = uv_udp_recv_start(&peer, on_alloc, on_recv);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, NULL, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &peer_addr);
  assert(e == 0);

  memset(&ack_packet, 0, sizeof(ack_packet));
  ack_packet.magic = UDX_MAGIC_BYTE;
  ack_packet.version = UDX_VERSION;
  ack_packet.remote_id = udx__swap_uint32_if_be(1);
  ack_packet.rwnd = udx__swap_uint32_if_be(UINT32_MAX);

  e = uv_timer_init(&loop, &ack_timer);
  assert(e == 0);

  e = uv_timer_init(&loop, &poll_timer);
  assert(e == 0);

  write_bytes(1);

  started = uv_now(&loop);
  uv_timer_start(&poll_timer, on_poll, 5, 5);
  uv_run(&loop, UV_RUN_DEFAULT);

  return 0;
}

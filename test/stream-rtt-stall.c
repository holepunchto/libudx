#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/cirbuf.h"
#include "../src/endian.h"
#include "../src/internal.h"

// Acks queued during a 15 s stall arrive back to back. Each gives a clamped
// sample of the same stall: only the first may feed the estimator.

#define BURST    8
#define STALL_MS 15000

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_stream_t stream;

uv_udp_t peer;

struct sockaddr_in sock_addr;

uint32_t seen[BURST + 1];
uint32_t srtt;
uint32_t rttvar;

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

  udx_packet_t *pkt = (udx_packet_t *) udx__cirbuf_get(&stream.outgoing, seq);
  assert(pkt != NULL);

  if (seq == 0) {
    pkt->time_sent -= 100; // a 100 ms first sample, so rttvar > 0
    send_ack(1);
    return;
  }

  // as if sent before the stall; move rack's newest send time too, or rack resends them
  pkt->time_sent -= STALL_MS;
  if (seq == 1) stream.rack_time_sent -= STALL_MS;
  if (seq < BURST) return;

  srtt = stream.srtt;
  rttvar = stream.rttvar;
  assert(rttvar > 0);

  for (uint32_t ack = 2; ack <= BURST + 1; ack++) send_ack(ack);
}

static void
write_bytes (size_t len, udx_stream_ack_cb on_ack) {
  char *data = malloc(len);
  assert(data != NULL);
  memset(data, 'a', len);

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  assert(req != NULL);

  uv_buf_t buf = uv_buf_init(data, len);
  udx_stream_write(req, &stream, &buf, 1, on_ack);
}

static void
on_burst_ack (udx_stream_write_t *req, int status, int unordered) {
  uv_stop(&loop);
}

static void
on_first_ack (udx_stream_write_t *req, int status, int unordered) {
  write_bytes(udx__max_payload(&stream) * BURST, on_burst_ack);
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

  struct sockaddr_in peer_addr;
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

  write_bytes(1, on_first_ack);

  uv_run(&loop, UV_RUN_DEFAULT);

  // exactly one clamped sample of srtt + 5 * rttvar
  assert(stream.srtt == (8 * srtt + 5 * rttvar) / 8);
  assert(stream.rttvar == 2 * rttvar);

  return 0;
}

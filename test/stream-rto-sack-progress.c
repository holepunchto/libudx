#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/cirbuf.h"
#include "../src/endian.h"
#include "../src/internal.h"

// Leave sequence 0 unacknowledged while SACKing sequences 1-9. RACK retries
// the gap, but that retry is also left unacknowledged. Continued SACK progress
// must not postpone fallback RTO recovery.

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_stream_t stream;

uv_udp_t peer;
uv_udp_send_t sack_reqs[9];
uv_timer_t send_sack_timer;
uv_timer_t check_rack_timer;
uv_timer_t check_recovery_timer;

int sacks_sent;

struct {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t data_offset;
  uint32_t remote_id;
  uint32_t rwnd;
  uint32_t seq;
  uint32_t ack;
  uint32_t sack_start;
  uint32_t sack_end;
} sack_packet;

static void
on_send (uv_udp_send_t *req, int status) {
  assert(status == 0);
}

static void
check_rack_retransmit (uv_timer_t *timer) {
  assert(stream.remote_acked == 0);
  assert(stream.sacks > 0);
  assert(stream.retransmit_count == 1);
}

static void
check_rto_recovery (uv_timer_t *timer) {
  assert(stream.remote_acked == 0);
  assert(stream.sacks == 9);
  assert(stream.rto_count == 1);

  uv_stop(&loop);
}

static void
send_sack (uv_timer_t *timer) {
  uint32_t seq = sacks_sent + 1;
  assert(udx__cirbuf_get(&stream.outgoing, seq) != NULL);

  sack_packet.sack_start = udx__swap_uint32_if_be(seq);
  sack_packet.sack_end = udx__swap_uint32_if_be(seq + 1);

  struct sockaddr_in destination;
  uv_ip4_addr("127.0.0.1", 8081, &destination);

  uv_buf_t buf = uv_buf_init((char *) &sack_packet, sizeof(sack_packet));
  int e = uv_udp_send(&sack_reqs[sacks_sent], &peer, &buf, 1, (struct sockaddr *) &destination, on_send);
  assert(e == 0);

  sacks_sent++;
  if (sacks_sent == 9) uv_timer_stop(&send_sack_timer);
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

  struct sockaddr_in socket_addr;
  uv_ip4_addr("127.0.0.1", 8081, &socket_addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &socket_addr, 0);
  assert(e == 0);

  e = uv_udp_init(&loop, &peer);
  assert(e == 0);

  struct sockaddr_in peer_addr;
  uv_ip4_addr("127.0.0.1", 8082, &peer_addr);
  e = uv_udp_bind(&peer, (struct sockaddr *) &peer_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, NULL, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &peer_addr);
  assert(e == 0);

  size_t data_len = udx__max_payload(&stream) * 10;
  char *data = malloc(data_len);
  assert(data != NULL);
  memset(data, 'a', data_len);

  udx_stream_write_t *write = malloc(udx_stream_write_sizeof(1));
  assert(write != NULL);

  uv_buf_t buf = uv_buf_init(data, data_len);
  e = udx_stream_write(write, &stream, &buf, 1, NULL);
  assert(e != 0);
  assert(stream.seq == 10);
  assert(uv_is_active((uv_handle_t *) &stream.timer));

  memset(&sack_packet, 0, sizeof(sack_packet));
  sack_packet.magic = UDX_MAGIC_BYTE;
  sack_packet.version = UDX_VERSION;
  sack_packet.type = UDX_HEADER_SACK;
  sack_packet.remote_id = udx__swap_uint32_if_be(1);
  sack_packet.rwnd = udx__swap_uint32_if_be(UINT32_MAX);

  e = uv_timer_init(&loop, &send_sack_timer);
  assert(e == 0);
  e = uv_timer_init(&loop, &check_rack_timer);
  assert(e == 0);
  e = uv_timer_init(&loop, &check_recovery_timer);
  assert(e == 0);

  uv_timer_start(&send_sack_timer, send_sack, 100, 100);
  uv_timer_start(&check_rack_timer, check_rack_retransmit, 175, 0);
  uv_timer_start(&check_recovery_timer, check_rto_recovery, 1400, 0);
  uv_run(&loop, UV_RUN_DEFAULT);

  return 0;
}

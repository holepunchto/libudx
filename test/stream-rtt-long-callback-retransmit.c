#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// an ack read late in a wakeup, after a long callback, holds the callback's time in its fresh rtt
// sample. that time must not make a retransmission's sample long enough to pass the rack check
// against rtt_min, and must not go into srtt.
// P1 is lost on a sack 25 ms in and retransmitted. 2 ms later the receiver sends data, an ack
// of P1 and an ack of P0-P9. the read callback of the data blocks for 300 ms, so both acks are
// read 300 ms into the wakeup. the ack of P1 came too soon to be for the retransmission and
// must not make rack mark P5-P9 lost.

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

uv_timer_t sack_timer;
uv_timer_t check_timer;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

char data[11000]; // P0-P9

typedef struct {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t data_offset;
  uint32_t id;
  uint32_t rwnd;
  uint32_t seq;
  uint32_t ack;
  uint32_t payload[2]; // the sack block of an ack, or the data
} packet_t;

static void
send_packet (uint8_t type, uint32_t seq, uint32_t ack, uint32_t sack_start, uint32_t sack_end) {
  static packet_t pkts[4];
  static udx_socket_send_t reqs[4];
  static int n = 0;

  assert(n < 4);
  packet_t *pkt = &pkts[n];

  pkt->magic = UDX_MAGIC_BYTE;
  pkt->version = UDX_VERSION;
  pkt->type = type;
  pkt->id = udx__swap_uint32_if_be(1);
  pkt->rwnd = 0xffffffffu;
  pkt->seq = udx__swap_uint32_if_be(seq);
  pkt->ack = udx__swap_uint32_if_be(ack);
  pkt->payload[0] = udx__swap_uint32_if_be(sack_start);
  pkt->payload[1] = udx__swap_uint32_if_be(sack_end);

  uv_buf_t buf = uv_buf_init((char *) pkt, sizeof(*pkt));
  udx_socket_send(&reqs[n++], &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, NULL);
}

static void
on_sack (uv_timer_t *timer) {
  // P0 acked and P2-P4 sacked: P1 is lost
  send_packet(UDX_HEADER_SACK, 0, 1, 2, 5);
}

static void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  if (!(buf->base[2] & UDX_HEADER_DATA)) return;

  uint32_t seq;
  memcpy(&seq, buf->base + 12, sizeof(seq));
  seq = udx__swap_uint32_if_be(seq);

  static int p1_seen = 0;
  if (seq == 0) uv_timer_start(&sack_timer, on_sack, 25, 0);
  if (seq != 1 || ++p1_seen < 2) return;

  // the retransmission of P1. answer it in a later millisecond of the loop time than its send stamp
  uint64_t until = uv_hrtime() + 2000000;
  while (uv_hrtime() < until)
    uv_sleep(1);

  send_packet(UDX_HEADER_DATA, 0, 1, 0, 0);
  send_packet(0, 1, 2, 0, 0);
  send_packet(0, 1, 10, 0, 0);
}

static void
on_check (uv_timer_t *timer) {
  printf("rtt-long-callback-retransmit: srtt=%u rtt_min=%u lost=%u\n", stream.srtt, udx_rtt_min(&stream), stream.lost);

  // the rtt is about 25 ms, and the acks of P1 and P5-P9 waited 300 ms to be read
  assert(stream.lost == 1);
  assert(stream.srtt < 100);

  uv_close((uv_handle_t *) &sack_timer, NULL);
  uv_close((uv_handle_t *) timer, NULL);
  udx_stream_destroy(&stream);
}

static void
on_read (udx_stream_t *s, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len <= 0) return;

  uv_sleep(300);

  // runs after the reads of both acks in this wakeup
  uv_timer_start(&check_timer, on_check, 0, 0);
}

static void
on_stream_close (udx_stream_t *s, int status) {
  udx_socket_close(&send_sock);
  udx_socket_close(&recv_sock);
}

int
main () {
  int e;

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &recv_sock, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18095, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18096, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_stream_read_start(&stream, on_read);
  assert(e == 0);

  e = udx_socket_recv_start(&recv_sock, on_recv);
  assert(e == 0);

  uv_timer_init(&loop, &sack_timer);
  uv_timer_init(&loop, &check_timer);

  uv_buf_t buf = uv_buf_init(data, sizeof(data));
  udx_stream_write(req, &stream, &buf, 1, NULL);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  e = uv_loop_close(&loop);
  assert(e == 0);

  free(req);

  return 0;
}

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "../include/udx.h"
#include "../src/endian.h"

// BBR must deduct packets that RACK marks lost from cwnd during fast recovery.
// Send P0-P7, SACK P2-P4 so RACK marks P1 lost and recovery starts, then also
// SACK P6-P7 so RACK marks P5 lost: cwnd must drop by the newly lost packets.

#define NPACKETS 8

uv_loop_t loop;
udx_t udx;
udx_socket_t sender;
udx_socket_t receiver;
udx_stream_t stream;
udx_stream_write_t *write_req;
uv_timer_t ack_timer;
struct sockaddr_in sender_addr;
struct sockaddr_in receiver_addr;

int transmits[NPACKETS];
int received = 0;
int step = 0;
uint32_t recovery_cwnd;
uint32_t recovery_lost;

uint32_t acks[3][9]; // header plus up to two SACK blocks, per ACK we send
udx_socket_send_t ack_reqs[3];

static void
send_ack (int i, uint32_t ack, const uint32_t *sacks, int nsacks) {
  uint8_t *b = (uint8_t *) acks[i];
  uint32_t *w = acks[i] + 1;
  b[0] = 0xff; // magic
  b[1] = 1;    // version
  b[2] = nsacks ? UDX_HEADER_SACK : 0;
  w[0] = udx__swap_uint32_if_be(1); // remote stream id
  w[1] = 0xffffffff;                // rwnd
  w[3] = udx__swap_uint32_if_be(ack);
  for (int j = 0; j < 2 * nsacks; j++) w[4 + j] = udx__swap_uint32_if_be(sacks[j]);
  uv_buf_t buf = uv_buf_init((char *) b, UDX_HEADER_SIZE + 8 * nsacks);
  int e = udx_socket_send(&ack_reqs[i], &receiver, &buf, 1, (struct sockaddr *) &sender_addr, NULL);
  assert(e >= 0);
}

static void
on_ack_timer (uv_timer_t *timer) {
  (void) timer;
  uint32_t sacks[] = {2, 5};
  send_ack(0, 1, sacks, 1); // ACK P0, SACK P2-P4: P1 is lost
  step = 1;
}

static void
on_recv (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) socket;
  (void) from;
  assert(read_len >= UDX_HEADER_SIZE);
  if (!((uint8_t) buf->base[2] & UDX_HEADER_DATA)) return;
  uint32_t seq = udx__swap_uint32_if_be(*(uint32_t *) (buf->base + 12));
  assert(seq < NPACKETS);

  if (++transmits[seq] == 1) {
    // the delay makes retransmissions leave in a later millisecond than P0-P7
    if (++received == NPACKETS) uv_timer_start(&ack_timer, on_ack_timer, 5, 0);
    return;
  }

  if (step == 1) { // the P1 retransmission
    assert(seq == 1);
    assert(stream.ca_state == UDX_CA_RECOVERY);
    recovery_cwnd = stream.cwnd;
    recovery_lost = stream.lost;
    uint32_t sacks[] = {2, 5, 6, 8};
    send_ack(1, 1, sacks, 2); // also SACK P6-P7: P5 is lost
    step = 2;
  } else if (step == 2) { // the retransmission that follows
    uint32_t lost = stream.lost - recovery_lost;
    assert(lost >= 1);
    assert(stream.cwnd == recovery_cwnd - lost);
    step = 3;
    send_ack(2, NPACKETS, NULL, 0); // ACK everything
  }
}

static void
on_write_ack (udx_stream_write_t *req, int status, int unordered) {
  (void) req;
  (void) unordered;
  assert(status == 0);
  udx_stream_destroy(&stream);
}

static void
on_stream_close (udx_stream_t *stream, int status) {
  (void) stream;
  (void) status;
  udx_socket_close(&sender);
  udx_socket_close(&receiver);
  uv_close((uv_handle_t *) &ack_timer, NULL);
}

int
main () {
  int e = uv_loop_init(&loop);
  assert(e == 0);
  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);
  e = udx_socket_init(&udx, &sender, NULL);
  assert(e == 0);
  e = udx_socket_init(&udx, &receiver, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18121, &sender_addr);
  uv_ip4_addr("127.0.0.1", 18122, &receiver_addr);
  e = udx_socket_bind(&sender, (struct sockaddr *) &sender_addr, 0);
  assert(e == 0);
  e = udx_socket_bind(&receiver, (struct sockaddr *) &receiver_addr, 0);
  assert(e == 0);
  e = udx_socket_recv_start(&receiver, on_recv);
  assert(e == 0);
  e = uv_timer_init(&loop, &ack_timer);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);
  e = udx_stream_connect(&stream, &sender, 2, (struct sockaddr *) &receiver_addr);
  assert(e == 0);

  write_req = malloc(udx_stream_write_sizeof(1));
  assert(write_req != NULL);
  char data[8800] = {0}; // 8 packets
  uv_buf_t buf = uv_buf_init(data, sizeof(data));
  e = udx_stream_write(write_req, &stream, &buf, 1, on_write_ack);
  assert(e != 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  e = uv_loop_close(&loop);
  assert(e == 0);
  assert(step == 3);

  free(write_req);
  return 0;
}

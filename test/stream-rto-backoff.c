// Keep the regression checks active in Release builds too.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/endian.h"

#define INITIAL_RTO_MS 1000
#define ACK_DELAY_MS   1500

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

uv_loop_t loop;
udx_t udx;
udx_socket_t sender;
udx_socket_t receiver;
udx_stream_t stream;
udx_socket_send_t ack_reqs[2];
uv_timer_t ack_timer;
uv_timer_t watchdog;

struct sockaddr_in sender_addr;
packet_header_t ack_packets[2];

int transmits[2];
int writes_acked;

static void
on_stream_close (udx_stream_t *stream, int status) {
  (void) stream;
  assert(status == 0);
  int err = udx_socket_close(&sender);
  assert(err == 0);
  err = udx_socket_close(&receiver);
  assert(err == 0);
}

static void
send_ack (uint32_t seq) {
  packet_header_t *packet = &ack_packets[seq];
  memset(packet, 0, sizeof(*packet));
  packet->magic = 0xff;
  packet->version = 1;
  packet->id = udx__swap_uint32_if_be(1);
  packet->rwnd = udx__swap_uint32_if_be(UINT32_MAX);
  packet->ack = udx__swap_uint32_if_be(seq + 1);

  uv_buf_t buf = uv_buf_init((char *) packet, sizeof(*packet));
  int err = udx_socket_send(&ack_reqs[seq], &receiver, &buf, 1, (struct sockaddr *) &sender_addr, NULL);
  assert(err == 0);
}

static void
send_delayed_ack (uv_timer_t *timer) {
  send_ack(1);
  uv_close((uv_handle_t *) timer, NULL);
}

static void
on_receiver_read (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) socket;
  (void) from;
  assert(read_len >= (ssize_t) sizeof(packet_header_t));

  packet_header_t *header = (packet_header_t *) buf->base;
  if (!(header->type & UDX_HEADER_DATA)) return;

  uint32_t seq = udx__swap_uint32_if_be(header->seq);
  assert(seq < 2);
  transmits[seq]++;

  // Wait for the first packet's retransmission before ACKing it. Delay the
  // second packet's ACK to fall between the initial and backed-off RTOs.
  if (seq == 0 && transmits[0] == 2) {
    send_ack(0);
  } else if (seq == 1 && transmits[1] == 1) {
    int err = uv_timer_start(&ack_timer, send_delayed_ack, ACK_DELAY_MS, 0);
    assert(err == 0);
  }
}

static void
on_write_acked (udx_stream_write_t *req, int status, int unordered) {
  (void) req;
  assert(status == 0);
  assert(unordered == 0);

  writes_acked++;
}

static void
on_timeout (uv_timer_t *timer) {
  (void) timer;
  fputs("RTO backoff test timed out\n", stderr);
  abort();
}

int
main () {
  int err = uv_loop_init(&loop);
  assert(err == 0);

  err = udx_init(&loop, &udx, NULL);
  assert(err == 0);
  err = udx_socket_init(&udx, &sender, NULL);
  assert(err == 0);
  err = udx_socket_init(&udx, &receiver, NULL);
  assert(err == 0);

  err = uv_ip4_addr("127.0.0.1", 0, &sender_addr);
  assert(err == 0);
  err = udx_socket_bind(&sender, (struct sockaddr *) &sender_addr, 0);
  assert(err == 0);
  int sender_addr_len = sizeof(sender_addr);
  err = udx_socket_getsockname(&sender, (struct sockaddr *) &sender_addr, &sender_addr_len);
  assert(err == 0);

  struct sockaddr_in receiver_addr;
  err = uv_ip4_addr("127.0.0.1", 0, &receiver_addr);
  assert(err == 0);
  err = udx_socket_bind(&receiver, (struct sockaddr *) &receiver_addr, 0);
  assert(err == 0);
  int receiver_addr_len = sizeof(receiver_addr);
  err = udx_socket_getsockname(&receiver, (struct sockaddr *) &receiver_addr, &receiver_addr_len);
  assert(err == 0);
  err = udx_socket_recv_start(&receiver, on_receiver_read);
  assert(err == 0);

  err = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(err == 0);
  err = udx_stream_connect(&stream, &sender, 2, (struct sockaddr *) &receiver_addr);
  assert(err == 0);
  stream.rto = INITIAL_RTO_MS;

  err = uv_timer_init(&loop, &ack_timer);
  assert(err == 0);
  err = uv_timer_init(&loop, &watchdog);
  assert(err == 0);
  err = uv_timer_start(&watchdog, on_timeout, 10000, 0);
  assert(err == 0);

  udx_stream_write_t *write_req = malloc(udx_stream_write_sizeof(1));
  assert(write_req != NULL);
  uv_buf_t buf = uv_buf_init("x", 1);
  err = udx_stream_write(write_req, &stream, &buf, 1, on_write_acked);
  assert(err > 0);

  // Let ACK processing finish before starting a subsequent write.
  while (writes_acked == 0) uv_run(&loop, UV_RUN_ONCE);

  // A real RTO occurred. Karn's rule excludes this ACK as an RTT sample,
  // so the backed-off RTO must carry over to the next write.
  assert(stream.rto_count == 1);
  assert(stream.srtt == 0);
  assert(stream.rto == 2 * INITIAL_RTO_MS);

  err = udx_stream_write(write_req, &stream, &buf, 1, on_write_acked);
  assert(err > 0);
  while (writes_acked == 1) uv_run(&loop, UV_RUN_ONCE);

  // The delayed ACK now provides a clean sample instead of another timeout.
  assert(writes_acked == 2);
  assert(transmits[1] == 1);
  assert(stream.srtt > INITIAL_RTO_MS);

  uv_close((uv_handle_t *) &watchdog, NULL);
  err = udx_stream_destroy(&stream);
  assert(err >= 0);
  err = uv_run(&loop, UV_RUN_DEFAULT);
  assert(err == 0);
  err = uv_loop_close(&loop);
  assert(err == 0);

  free(write_req);
  return 0;
}

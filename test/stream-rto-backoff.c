#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/endian.h"

#define INITIAL_RTO_MS 1000
#define ACK_DELAY_MS   1050

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
udx_stream_write_t *write_reqs[2];
udx_socket_send_t ack_reqs[2];
uv_timer_t ack_timers[2];
uv_timer_t next_write_timer;
uv_timer_t watchdog;

struct sockaddr_in sender_addr;
struct sockaddr_in receiver_addr;
packet_header_t ack_packets[2];

char payloads[2] = {1, 2};
int transmits[2];
int writes_acked;
bool ack_scheduled[2];
bool completed;

static void
on_write_acked (udx_stream_write_t *req, int status, int unordered);

static void
on_socket_close (udx_socket_t *socket) {
  (void) socket;
}

static void
on_stream_close (udx_stream_t *stream, int status) {
  (void) stream;
  assert(status == 0);
  assert(udx_socket_close(&sender) == 0);
  assert(udx_socket_close(&receiver) == 0);
}

static void
on_ack_sent (udx_socket_send_t *req, int status) {
  (void) req;
  assert(status == 0);
}

static void
send_delayed_ack (uv_timer_t *timer) {
  uintptr_t seq = (uintptr_t) timer->data;
  assert(seq < 2);

  packet_header_t *packet = &ack_packets[seq];
  memset(packet, 0, sizeof(*packet));
  packet->magic = 0xff;
  packet->version = 1;
  packet->id = udx__swap_uint32_if_be(1);
  packet->rwnd = udx__swap_uint32_if_be(UINT32_MAX);
  packet->ack = udx__swap_uint32_if_be(seq + 1);

  uv_buf_t buf = uv_buf_init((char *) packet, sizeof(*packet));
  int err = udx_socket_send(&ack_reqs[seq], &receiver, &buf, 1, (struct sockaddr *) &sender_addr, on_ack_sent);
  assert(err == 0);

  uv_timer_stop(timer);
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

  if (!ack_scheduled[seq]) {
    ack_scheduled[seq] = true;
    int err = uv_timer_start(&ack_timers[seq], send_delayed_ack, ACK_DELAY_MS, 0);
    assert(err == 0);
  }
}

static void
write_second_packet (uv_timer_t *timer) {
  assert(stream.remote_acked == 1);

  uv_timer_stop(timer);
  uv_close((uv_handle_t *) timer, NULL);

  uv_buf_t buf = uv_buf_init(&payloads[1], 1);
  int err = udx_stream_write(write_reqs[1], &stream, &buf, 1, on_write_acked);
  assert(err > 0);
}

static void
on_write_acked (udx_stream_write_t *req, int status, int unordered) {
  (void) req;
  assert(status == 0);
  assert(unordered == 0);

  if (writes_acked++ == 0) {
    // The first packet was retransmitted, so Karn's algorithm correctly
    // leaves the RTT estimator uninitialized. The backed-off RTO must be
    // retained for the next packet instead of reverting to INITIAL_RTO_MS.
    assert(transmits[0] == 2);
    assert(stream.retransmit_count == 1);
    assert(stream.srtt == 0);
    assert(stream.rto == 2 * INITIAL_RTO_MS);

    int err = uv_timer_start(&next_write_timer, write_second_packet, 0, 0);
    assert(err == 0);
    return;
  }

  // The second ACK arrives after the initial RTO but before the persisted
  // backoff. It therefore acknowledges a packet that was never retransmitted
  // and bootstraps the RTT estimator.
  assert(transmits[1] == 1);
  assert(stream.retransmit_count == 1);
  assert(stream.srtt > INITIAL_RTO_MS);
  assert(stream.rto >= 1000);

  completed = true;
  uv_timer_stop(&watchdog);
  uv_close((uv_handle_t *) &watchdog, NULL);
  assert(udx_stream_destroy(&stream) >= 0);
}

static void
on_timeout (uv_timer_t *timer) {
  (void) timer;
  assert(false && "RTO backoff test timed out");
}

int
main () {
  int err = uv_loop_init(&loop);
  assert(err == 0);

  err = udx_init(&loop, &udx, NULL);
  assert(err == 0);
  err = udx_socket_init(&udx, &sender, on_socket_close);
  assert(err == 0);
  err = udx_socket_init(&udx, &receiver, on_socket_close);
  assert(err == 0);

  err = uv_ip4_addr("127.0.0.1", 0, &sender_addr);
  assert(err == 0);
  err = udx_socket_bind(&sender, (struct sockaddr *) &sender_addr, 0);
  assert(err == 0);
  int sender_addr_len = sizeof(sender_addr);
  err = udx_socket_getsockname(&sender, (struct sockaddr *) &sender_addr, &sender_addr_len);
  assert(err == 0);

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

  for (uintptr_t seq = 0; seq < 2; seq++) {
    err = uv_timer_init(&loop, &ack_timers[seq]);
    assert(err == 0);
    ack_timers[seq].data = (void *) seq;

    write_reqs[seq] = malloc(udx_stream_write_sizeof(1));
    assert(write_reqs[seq] != NULL);
  }

  err = uv_timer_init(&loop, &next_write_timer);
  assert(err == 0);
  err = uv_timer_init(&loop, &watchdog);
  assert(err == 0);
  err = uv_timer_start(&watchdog, on_timeout, 5000, 0);
  assert(err == 0);

  uv_buf_t buf = uv_buf_init(&payloads[0], 1);
  err = udx_stream_write(write_reqs[0], &stream, &buf, 1, on_write_acked);
  assert(err > 0);

  err = uv_run(&loop, UV_RUN_DEFAULT);
  assert(err == 0);
  err = uv_loop_close(&loop);
  assert(err == 0);

  assert(completed);
  assert(writes_acked == 2);

  free(write_reqs[0]);
  free(write_reqs[1]);
  return 0;
}

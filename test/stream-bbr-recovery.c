#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/endian.h"

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
  packet_header_t header;
  uint32_t sack_start;
  uint32_t sack_end;
  uint8_t payload;
} sack_packet_t;

typedef struct {
  packet_header_t header;
  uint8_t payload;
} data_packet_t;

enum {
  RECV_INITIAL_DATA,
  RECV_SACK_ACK,
  RECV_FINAL_ACK,
};

uv_loop_t loop;
udx_t udx;
udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;
udx_socket_send_t sack_req;
udx_socket_send_t ack_req;
uv_timer_t timeout;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

sack_packet_t sack_packet;
data_packet_t ack_packet;
int recv_phase = RECV_INITIAL_DATA;
bool write_acked;

static void
on_socket_close (udx_socket_t *socket) {
  (void) socket;
}

static void
on_stream_close (udx_stream_t *stream, int status) {
  (void) stream;
  assert(status == 0);
  assert(udx_socket_close(&send_sock) == 0);
  assert(udx_socket_close(&recv_sock) == 0);
}

static void
on_packet_sent (udx_socket_send_t *req, int status) {
  (void) req;
  assert(status == 0);
}

static void
on_write_acked (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  assert(unordered == 1);
  write_acked = true;
  free(req);
}

static packet_header_t
make_header (uint8_t type, uint8_t data_offset, uint32_t seq, uint32_t ack) {
  packet_header_t header;
  memset(&header, 0, sizeof(header));
  header.magic = 0xff;
  header.version = 1;
  header.type = type;
  header.data_offset = data_offset;
  header.id = udx__swap_uint32_if_be(1);
  header.rwnd = udx__swap_uint32_if_be(UINT32_MAX);
  header.seq = udx__swap_uint32_if_be(seq);
  header.ack = udx__swap_uint32_if_be(ack);
  return header;
}

static void
send_sack () {
  memset(&sack_packet, 0, sizeof(sack_packet));
  sack_packet.header = make_header(UDX_HEADER_DATA | UDX_HEADER_SACK, 2 * sizeof(uint32_t), 0, 0);
  sack_packet.sack_start = udx__swap_uint32_if_be(0);
  sack_packet.sack_end = udx__swap_uint32_if_be(1);
  sack_packet.payload = 1;

  uv_buf_t buf = uv_buf_init((char *) &sack_packet, offsetof(sack_packet_t, payload) + 1);
  int err = udx_socket_send(&sack_req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_packet_sent);
  assert(err == 0);
}

static void
send_cumulative_ack () {
  memset(&ack_packet, 0, sizeof(ack_packet));
  ack_packet.header = make_header(UDX_HEADER_DATA, 0, 1, 1);
  ack_packet.payload = 1;

  uv_buf_t buf = uv_buf_init((char *) &ack_packet, offsetof(data_packet_t, payload) + 1);
  int err = udx_socket_send(&ack_req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_packet_sent);
  assert(err == 0);
}

static void
on_recv (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) socket;
  (void) from;
  assert(read_len >= (ssize_t) sizeof(packet_header_t));

  packet_header_t *header = (packet_header_t *) buf->base;
  uint32_t seq = udx__swap_uint32_if_be(header->seq);
  uint32_t ack = udx__swap_uint32_if_be(header->ack);

  if (recv_phase == RECV_INITIAL_DATA) {
    assert(header->type & UDX_HEADER_DATA);
    assert(seq == 0);
    assert(stream.seq == 1);
    assert(stream.remote_acked == 0);

    // Model a packet sent during recovery. The peer first SACKs that packet,
    // then advances its cumulative ACK. The second packet exits recovery but
    // does not newly acknowledge anything, so BBR must still process the
    // congestion-state transition and restore its saved window.
    stream.high_seq = 0;
    stream.ca_state = UDX_CA_RECOVERY;
    stream.cwnd = 4;
    stream.ssthresh = 0;
    stream.bbr.prior_cwnd = 10;
    stream.bbr.prev_ca_state = UDX_CA_RECOVERY;
    stream.bbr.use_packet_conservation = true;

    recv_phase = RECV_SACK_ACK;
    send_sack();
    return;
  }

  if (recv_phase == RECV_SACK_ACK) {
    assert(header->type == 0);
    assert(ack == 1);
    assert(write_acked);
    assert(stream.remote_acked == 0);
    assert(stream.sacks == 1);
    assert(stream.inflight_queue.len == 0);
    assert(stream.ca_state == UDX_CA_RECOVERY);

    recv_phase = RECV_FINAL_ACK;
    send_cumulative_ack();
    return;
  }

  assert(recv_phase == RECV_FINAL_ACK);
  assert(header->type == 0);
  assert(ack == 2);
  assert(stream.remote_acked == 1);
  assert(stream.sacks == 0);
  assert(stream.inflight_queue.len == 0);
  assert(stream.retransmit_queue.len == 0);
  assert(stream.ca_state == UDX_CA_OPEN);
  assert(stream.cwnd == 10);
  assert(stream.bbr.prev_ca_state == UDX_CA_OPEN);
  assert(stream.bbr.use_packet_conservation == false);

  uv_timer_stop(&timeout);
  uv_close((uv_handle_t *) &timeout, NULL);
  assert(udx_stream_destroy(&stream) >= 0);
}

static void
on_timeout (uv_timer_t *timer) {
  (void) timer;
  assert(false && "recovery did not finish");
}

int
main () {
  int err = uv_loop_init(&loop);
  assert(err == 0);

  err = udx_init(&loop, &udx, NULL);
  assert(err == 0);
  err = udx_socket_init(&udx, &send_sock, on_socket_close);
  assert(err == 0);
  err = udx_socket_init(&udx, &recv_sock, on_socket_close);
  assert(err == 0);

  err = uv_ip4_addr("127.0.0.1", 0, &send_addr);
  assert(err == 0);
  err = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(err == 0);
  int send_addr_len = sizeof(send_addr);
  err = udx_socket_getsockname(&send_sock, (struct sockaddr *) &send_addr, &send_addr_len);
  assert(err == 0);

  err = uv_ip4_addr("127.0.0.1", 0, &recv_addr);
  assert(err == 0);
  err = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(err == 0);
  int recv_addr_len = sizeof(recv_addr);
  err = udx_socket_getsockname(&recv_sock, (struct sockaddr *) &recv_addr, &recv_addr_len);
  assert(err == 0);

  err = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(err == 0);
  err = udx_stream_connect(&stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(err == 0);
  err = udx_socket_recv_start(&recv_sock, on_recv);
  assert(err == 0);

  udx_stream_write_t *write_req = malloc(udx_stream_write_sizeof(1));
  assert(write_req != NULL);
  char payload = 1;
  uv_buf_t buf = uv_buf_init(&payload, 1);
  err = udx_stream_write(write_req, &stream, &buf, 1, on_write_acked);
  assert(err > 0);

  err = uv_timer_init(&loop, &timeout);
  assert(err == 0);
  err = uv_timer_start(&timeout, on_timeout, 5000, 0);
  assert(err == 0);

  err = uv_run(&loop, UV_RUN_DEFAULT);
  assert(err == 0);
  err = uv_loop_close(&loop);
  assert(err == 0);

  return 0;
}

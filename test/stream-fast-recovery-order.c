#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/endian.h"
#include "../src/internal.h"

// The first recovery retransmits sequences 0-4 and moves them behind 6-9 in
// the inflight queue. A later loss then checks that recovery prioritizes
// sequence 0, which is blocking the receiver's cumulative ACK.

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

udx_stream_write_t *initial_write;
udx_stream_write_t *later_write;
char *initial_data;
char *later_data;

udx_socket_send_t sack_reqs[2];

struct sack_packet {
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
} sack_packets[2];

enum {
  RECOVERY_CWND = 4
};

enum {
  WAIT_INITIAL,
  WAIT_FIRST_RECOVERY,
  WAIT_LATER_PACKET,
  WAIT_SECOND_RECOVERY,
  DONE
} stage;

int transmits[32];
int sacks_sent;
uint32_t later_seq;
uint32_t second_recovery_packets;
bool blocking_packet_retransmitted;

#if defined(_WIN32)
static void
sleep_ms (long milliseconds) {
  Sleep(milliseconds);
}
#else
#include <time.h>
static void
sleep_ms (long milliseconds) {
  struct timespec ts;
  ts.tv_sec = milliseconds / 1000;
  ts.tv_nsec = (milliseconds % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}
#endif

static void
on_socket_close (udx_socket_t *socket) {
  (void) socket;
}

static void
on_stream_close (udx_stream_t *s, int status) {
  (void) s;
  (void) status;

  udx_socket_close(&send_sock);
  udx_socket_close(&recv_sock);
}

static void
on_sack_sent (udx_socket_send_t *req, int status) {
  (void) req;
  assert(status == 0);
}

static void
send_sack (uint32_t seq) {
  assert(sacks_sent < 2);
  struct sack_packet *pkt = &sack_packets[sacks_sent];
  memset(pkt, 0, sizeof(*pkt));

  pkt->magic = UDX_MAGIC_BYTE;
  pkt->version = UDX_VERSION;
  pkt->type = UDX_HEADER_SACK;
  pkt->remote_id = udx__swap_uint32_if_be(1);
  pkt->rwnd = udx__swap_uint32_if_be(UINT32_MAX);
  pkt->sack_start = udx__swap_uint32_if_be(seq);
  pkt->sack_end = udx__swap_uint32_if_be(seq + 1);

  uv_buf_t buf = uv_buf_init((char *) pkt, sizeof(*pkt));
  int e = udx_socket_send(&sack_reqs[sacks_sent], &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_sack_sent);
  assert(e == 0);
  sacks_sent++;
}

static bool
received_range (uint32_t start, uint32_t end, int count) {
  for (uint32_t seq = start; seq < end; seq++) {
    if (transmits[seq] != count) return false;
  }
  return true;
}

static void
on_recv (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) socket;
  (void) from;
  assert(read_len >= 20);

  uint8_t type = (uint8_t) buf->base[2];
  if (!(type & UDX_HEADER_DATA)) return;

  uint32_t *header = (uint32_t *) (buf->base + 4);
  uint32_t seq = header[2];

  assert(seq < 32);
  transmits[seq]++;

  if (stage == WAIT_INITIAL && received_range(0, 10, 1)) {
    // SACK sequence 5 so RACK retransmits the earlier 0-4 gap.
    stage = WAIT_FIRST_RECOVERY;
    sleep_ms(2);
    send_sack(5);
    return;
  }

  if (stage == WAIT_FIRST_RECOVERY && received_range(0, 5, 2)) {
    // Send a newer packet after 0-4 have moved to the inflight queue's tail.
    stage = WAIT_LATER_PACKET;

    uv_buf_t later = uv_buf_init(later_data, udx__max_payload(&stream));
    int e = udx_stream_write(later_write, &stream, &later, 1, NULL);
    assert(e != 0);
    later_seq = stream.seq - 1;
    return;
  }

  if (stage == WAIT_LATER_PACKET && seq == later_seq) {
    // Four is BBR's minimum cwnd and is smaller than the pending loss set.
    stream.cwnd = RECOVERY_CWND;
    stage = WAIT_SECOND_RECOVERY;
    sleep_ms(2);
    send_sack(later_seq);
    return;
  }

  if (stage == WAIT_SECOND_RECOVERY) {
    // A full recovery window without sequence 0 cannot advance the cumulative ACK.
    if (seq == 0) blocking_packet_retransmitted = true;
    if (++second_recovery_packets < RECOVERY_CWND) return;

    stage = DONE;
    udx_stream_destroy(&stream);
  }
}

int
main () {
  int e = uv_loop_init(&loop);
  assert(e == 0);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, on_socket_close);
  assert(e == 0);
  e = udx_socket_init(&udx, &recv_sock, on_socket_close);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18081, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18082, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);
  e = udx_stream_connect(&stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  // Path MTU discovery is unrelated and can change the number of test packets.
  stream.mtu_state = UDX_MTU_STATE_SEARCH_COMPLETE;
  stream.mtu_probe_wanted = false;

  e = udx_socket_recv_start(&recv_sock, on_recv);
  assert(e == 0);

  size_t packet_size = udx__max_payload(&stream);
  initial_data = calloc(10, packet_size);
  later_data = calloc(1, packet_size);
  initial_write = malloc(udx_stream_write_sizeof(1));
  later_write = malloc(udx_stream_write_sizeof(1));
  assert(initial_data != NULL && later_data != NULL);
  assert(initial_write != NULL && later_write != NULL);

  uv_buf_t initial = uv_buf_init(initial_data, packet_size * 10);
  e = udx_stream_write(initial_write, &stream, &initial, 1, NULL);
  assert(e != 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  e = uv_loop_close(&loop);
  assert(e == 0);

  assert(stage == DONE);
  assert(blocking_packet_retransmitted);

  free(initial_write);
  free(later_write);
  free(initial_data);
  free(later_data);
  return 0;
}

#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// out-of-order END must not let the late closer skip TIME-WAIT
//
// emulated peer = the active closer: it sent data (seq 0) then END (seq 1), and the data
// packet was lost, so the real stream sees END out of order (ENDING_REMOTE, not ENDED_REMOTE).
//
// 1. sender (emulated) sends END seq=1. real stream SACKs it; its ack is stuck at 0 (gap).
// 2. real stream calls write_end(). its END (seq 0) carries ack=0, so it does NOT ack the peer's END.
// 3. sender (emulated) fills the gap (data seq 0) and acks the real stream's END (ack=1).
// 4. real stream sends ACK(2) covering the peer's END and closes.
// 5. ACK(2) is "lost": peer retransmits data seq 0. the real stream must answer from TIME-WAIT,
//    otherwise the peer's END is never acked and the peer eventually fails with ETIMEDOUT.

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

udx_stream_write_t *end_req;
udx_socket_send_t send_req;
uv_timer_t fail_timer;

bool write_end_called;
bool timewait_wanted_after_write_end;
bool replied_to_end;
int final_acks_seen;
int close_status = -1;
bool sockets_closed;

struct hdr {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t data_offset;
  uint32_t id;
  uint32_t rwnd;
  uint32_t seq;
  uint32_t ack;
  char data[1];
};

static void
on_socket_close (udx_socket_t *socket) {
  (void) socket;
}

static void
on_stream_close (udx_stream_t *s, int status) {
  (void) s;
  close_status = status;
  printf("  real stream closed status=%d\n", status);
}

static void
on_sent (udx_socket_send_t *req, int status) {
  (void) req;
  (void) status;
}

static void
peer_send (int type, uint32_t seq, uint32_t ack, bool with_data) {
  static struct hdr pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.magic = 0xff;
  pkt.version = 1;
  pkt.type = type;
  pkt.id = udx__swap_uint32_if_be(1);
  pkt.rwnd = 0xffffffffu;
  pkt.seq = udx__swap_uint32_if_be(seq);
  pkt.ack = udx__swap_uint32_if_be(ack);
  pkt.data[0] = 'x';
  uv_buf_t buf = uv_buf_init((char *) &pkt, with_data ? sizeof(pkt) : UDX_HEADER_SIZE);
  udx_socket_send(&send_req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_sent);
}

static void
finish (void) {
  if (sockets_closed) return;
  sockets_closed = true;
  uv_timer_stop(&fail_timer);
  uv_close((uv_handle_t *) &fail_timer, NULL);
  udx_socket_close(&send_sock);
  udx_socket_close(&recv_sock);
}

static void
on_fail_timeout (uv_timer_t *t) {
  (void) t;
  printf("  no ACK retransmitted within 1s -- final ACK is unrecoverable\n");
  finish();
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  (void) req;
  (void) status;
  (void) unordered;
}

static void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) handle;
  (void) from;
  if (read_len < UDX_HEADER_SIZE) return;

  uint8_t type = (uint8_t) buf->base[2];
  uint32_t seq = udx__swap_uint32_if_be(*(uint32_t *) (buf->base + 12));
  uint32_t ack = udx__swap_uint32_if_be(*(uint32_t *) (buf->base + 16));
  printf("  peer rx: type=0x%x seq=%u ack=%u status=0x%x\n", type, seq, ack, stream.status);

  // (2) real stream SACKed our out-of-order END; now the app calls write_end()
  if (!write_end_called && (type & UDX_HEADER_SACK) && ack == 0) {
    assert(stream.status & UDX_STREAM_ENDING_REMOTE);
    assert(!(stream.status & UDX_STREAM_ENDED_REMOTE));
    write_end_called = true;
    int e = udx_stream_write_end(end_req, &stream, NULL, 0, on_ack);
    assert(e >= 0);
    timewait_wanted_after_write_end = stream.status & UDX_STREAM_TIMEWAIT_WANTED;
    printf("  write_end() called; TIMEWAIT_WANTED=%d\n", timewait_wanted_after_write_end);
    return;
  }

  // (3) real stream's END arrives with ack=0 (does not cover our END). fill the gap + ack its END.
  if ((type & UDX_HEADER_END) && !replied_to_end) {
    replied_to_end = true;
    peer_send(UDX_HEADER_DATA, 0, 1, true);
    return;
  }

  // (4) the ACK covering our END
  if (ack == 2) {
    final_acks_seen++;
    if (final_acks_seen == 1) {
      // pretend it was lost: retransmit the data packet and wait for a re-ACK
      uv_timer_start(&fail_timer, on_fail_timeout, 1000, 0);
      peer_send(UDX_HEADER_DATA, 0, 1, true);
    } else {
      printf("  final ACK retransmitted from TIME-WAIT\n");
      finish();
    }
  }
}

int
main (int argc, char **argv) {
  (void) argc;
  (void) argv;
  int e;

  end_req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);
  uv_timer_init(&loop, &fail_timer);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);
  e = udx_socket_init(&udx, &send_sock, on_socket_close);
  assert(e == 0);
  e = udx_socket_init(&udx, &recv_sock, on_socket_close);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18091, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);
  uv_ip4_addr("127.0.0.1", 18092, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);
  e = udx_stream_connect(&stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);
  e = udx_socket_recv_start(&recv_sock, on_recv);
  assert(e == 0);

  // (1) peer's END at seq 1 arrives; its data at seq 0 was "lost"
  peer_send(UDX_HEADER_END, 1, 0, false);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  assert(close_status == 0);
  assert(timewait_wanted_after_write_end);
  assert(final_acks_seen >= 2); // final ACK was retransmitted from TIME-WAIT

  e = uv_loop_close(&loop);
  assert(e == 0);

  free(end_req);

  return 0;
}

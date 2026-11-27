#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

// The receiver shrinks the window to zero with P1 and P2 in flight: it ACKs
// P0, SACKs P3 and advertises rwnd=0. The sender should still retransmit both
// P1 and P2, not only the oldest unacked packet like Linux does, and count
// them as zero window retransmits. Same fake receiver as stream-rack-tlp-example.

int event = 1;
int transmits[4];

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

udx_socket_send_t ack_req;

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
on_ack_sent (udx_socket_send_t *req, int status) {
  (void) req;
  (void) status;
}

static void
send_ack (uint32_t ack, uint32_t rwnd, bool sack) {
  struct {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t data_offset;
    uint32_t id;
    uint32_t rwnd;
    uint32_t seq;
    uint32_t ack;
    struct {
      uint32_t start;
      uint32_t end;
    } sack;
  } pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.magic = 0xff;
  pkt.version = 1;
  pkt.type = sack ? UDX_HEADER_SACK : 0;
  pkt.id = udx__swap_uint32_if_be(1);
  pkt.rwnd = udx__swap_uint32_if_be(rwnd);
  pkt.ack = udx__swap_uint32_if_be(ack);
  if (sack) {
    pkt.sack.start = udx__swap_uint32_if_be(3);
    pkt.sack.end = udx__swap_uint32_if_be(4);
  }

  uv_buf_t buf = uv_buf_init((char *) &pkt, sizeof(pkt));
  udx_socket_send(&ack_req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_ack_sent);
}

static void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  uint8_t type = ((uint8_t *) buf->base)[2];
  if (!(type & UDX_HEADER_DATA)) return;

  uint32_t seq = udx__swap_uint32_if_be(((uint32_t *) buf->base)[3]);
  assert(seq < 4);
  transmits[seq]++;

  if (event == 1) {
    if (transmits[0] == 1 && transmits[1] == 1 && transmits[2] == 1 && transmits[3] == 1) {
      assert(stream.send_rwnd > 0);
      send_ack(1, 0, true);
      event = 2;
    }
    return;
  }

  if (event == 2 && transmits[1] >= 2 && transmits[2] >= 2) {
    // P2 is past remote_acked, so this is more than an SND.UNA retransmit
    assert(stream.remote_acked == 1);
    assert(stream.send_rwnd == 0);
    assert(stream.zero_window_retransmit_count >= 2);
    assert(stream.zero_window_retransmit_count == stream.retransmit_count);

    send_ack(4, 0xffffffffu, false);
    event = 3;
  }
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  udx_stream_destroy(&stream);
}

int
main (int argc, char **argv) {
  int e;
  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, on_socket_close);
  assert(e == 0);

  e = udx_socket_init(&udx, &recv_sock, on_socket_close);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18161, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18162, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_socket_recv_start(&recv_sock, on_recv);
  assert(e == 0);

  int payload_size = 4400; // 4 packets

  char *data = calloc(1, payload_size);
  assert(data != NULL);

  uv_buf_t buf = uv_buf_init(data, payload_size);
  e = udx_stream_write(req, &stream, &buf, 1, on_ack);
  assert(e);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  e = uv_loop_close(&loop);
  assert(e == 0);

  free(req);
  free(data);

  assert(stream.bytes_acked == buf.len);
  assert(stream.zero_window_retransmit_count >= 2);

  return 0;
}

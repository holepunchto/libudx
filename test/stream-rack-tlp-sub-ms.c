#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// on a path that acks in under a millisecond, the sack for a tlp arrives in the same millisecond.
// rack must still use it to mark the earlier lost packet, so the loss is repaired without an rto.

#define WARM   20 // round trips before the first flight, so one reads 0 ms even on a slow host
#define ROUNDS 5 // losses, each after one more round trip so the next tlp is permitted

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

udx_stream_write_t *req;

char data[2000]; // two packets

int writes = 0;

uint32_t expected = 0; // next seq the receiver acks
int p1_transmits = 0;
int p2_transmits = 0;

static void
send_ack (uint32_t ack, uint32_t sack) {
  struct {
    uint8_t magic, version, type, data_offset;
    uint32_t id, rwnd, seq, ack;
    uint32_t sack_start, sack_end;
  } pkt;

  memset(&pkt, 0, sizeof(pkt));
  pkt.magic = 0xff;
  pkt.version = 1;
  pkt.id = udx__swap_uint32_if_be(1);
  pkt.rwnd = 0xffffffffu;
  pkt.ack = udx__swap_uint32_if_be(ack);

  size_t len = UDX_HEADER_SIZE;

  if (sack) {
    pkt.type = UDX_HEADER_SACK;
    pkt.sack_start = udx__swap_uint32_if_be(sack);
    pkt.sack_end = udx__swap_uint32_if_be(sack + 1);
    len = sizeof(pkt);
  }

  uv_buf_t buf = uv_buf_init((char *) &pkt, len);
  int e = uv_udp_try_send(&recv_sock.uv_udp, &buf, 1, (struct sockaddr *) &send_addr);
  assert(e == (int) len);
}

static void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  if (read_len < UDX_HEADER_SIZE || !(buf->base[2] & UDX_HEADER_DATA)) return;

  uint32_t seq = udx__swap_uint32_if_be(*(uint32_t *) (buf->base + 12));
  ssize_t len = read_len - UDX_HEADER_SIZE;

  if (len == 1) {
    if (seq == expected) expected++;
    send_ack(expected, 0);
    return;
  }

  // a two packet flight: the first transmission of both is lost
  if (seq == expected + 1 && ++p2_transmits == 2) {
    send_ack(expected, expected + 1); // the tlp, sacked at once
  } else if (seq == expected && ++p1_transmits == 2) {
    expected += 2;
    p1_transmits = p2_transmits = 0;
    send_ack(expected, 0); // the repair
  }
}

static void
on_ack (udx_stream_write_t *r, int status, int unordered);

static void
write_next () {
  bool flight = writes >= WARM && (writes - WARM) % 2 == 1;
  uv_buf_t buf = uv_buf_init(data, flight ? sizeof(data) : 1);
  int e = udx_stream_write(req, &stream, &buf, 1, on_ack);
  assert(e >= 0);
}

static void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  assert(status == 0);
  assert(stream.lifetime_rto_count == 0);

  if (++writes < WARM + 2 * ROUNDS) {
    write_next();
    return;
  }

  udx_stream_destroy(&stream);
}

static void
on_stream_close (udx_stream_t *s, int status) {
  udx_socket_close(&send_sock);
  udx_socket_close(&recv_sock);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &recv_sock, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18171, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18172, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_socket_recv_start(&recv_sock, on_recv);
  assert(e == 0);

  req = malloc(udx_stream_write_sizeof(1));

  write_next();

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  free(req);

  assert(writes == WARM + 2 * ROUNDS);

  return 0;
}

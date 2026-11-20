#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/endian.h"

// The stall timer stops once nothing is in flight, and the next write starts it again.
// A write after a pause longer than the timer's period plus threshold is not a stall.

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_stream_t stream;

uv_udp_t peer;
uv_timer_t idle_timer;

struct sockaddr_in sock_addr;

struct {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t data_offset;
  uint32_t remote_id;
  uint32_t rwnd;
  uint32_t seq;
  uint32_t ack;
} ack_packet;

static void
on_alloc (uv_handle_t *handle, size_t size, uv_buf_t *buf) {
  static char data[2048];
  *buf = uv_buf_init(data, sizeof(data));
}

static void
on_recv (uv_udp_t *handle, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags) {
  if (nread < 20 || !(buf->base[2] & UDX_HEADER_DATA)) return;

  uint32_t seq;
  memcpy(&seq, buf->base + 12, 4);

  // ack it right away
  ack_packet.ack = udx__swap_uint32_if_be(udx__swap_uint32_if_be(seq) + 1);

  uv_buf_t ack = uv_buf_init((char *) &ack_packet, sizeof(ack_packet));
  int e = uv_udp_try_send(&peer, &ack, 1, (struct sockaddr *) &sock_addr);
  assert(e == sizeof(ack_packet));
}

static void
write_byte (udx_stream_ack_cb on_ack) {
  static char data[1] = {'a'};

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  assert(req != NULL);

  uv_buf_t buf = uv_buf_init(data, 1);
  udx_stream_write(req, &stream, &buf, 1, on_ack);
}

static void
on_second_ack (udx_stream_write_t *req, int status, int unordered) {
  uv_stop(&loop);
}

static void
on_idle (uv_timer_t *timer) {
  assert(!uv_is_active((uv_handle_t *) &sock.stall_timer)); // stopped: nothing in flight

  write_byte(on_second_ack);
}

static void
on_first_ack (udx_stream_write_t *req, int status, int unordered) {
  uv_timer_start(&idle_timer, on_idle, 1000, 0); // longer than the stall timer's period plus threshold
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

  uv_ip4_addr("127.0.0.1", 18221, &sock_addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &sock_addr, 0);
  assert(e == 0);

  e = uv_udp_init(&loop, &peer);
  assert(e == 0);

  struct sockaddr_in peer_addr;
  uv_ip4_addr("127.0.0.1", 18222, &peer_addr);
  e = uv_udp_bind(&peer, (struct sockaddr *) &peer_addr, 0);
  assert(e == 0);

  e = uv_udp_recv_start(&peer, on_alloc, on_recv);
  assert(e == 0);

  e = uv_timer_init(&loop, &idle_timer);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, NULL, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &peer_addr);
  assert(e == 0);

  ack_packet.magic = UDX_MAGIC_BYTE;
  ack_packet.version = UDX_VERSION;
  ack_packet.remote_id = udx__swap_uint32_if_be(1);
  ack_packet.rwnd = udx__swap_uint32_if_be(UINT32_MAX);

  write_byte(on_first_ack);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(udx.stall_detected_ts == 0); // the pause was not a stall

  return 0;
}

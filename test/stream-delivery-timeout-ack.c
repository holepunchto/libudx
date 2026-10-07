#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/endian.h"
#include "../src/internal.h"

// an ack that advances restarts the delivery timeout

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_stream_t stream;

uv_udp_t peer;
uv_udp_send_t ack_req;
uv_timer_t ack_timer;

struct sockaddr_in sock_addr;
uint64_t acked;

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

void
on_close (udx_stream_t *s, int status) {
  assert(status == UV_ETIMEDOUT);
  assert(uv_now(&loop) - acked >= 3000);

  udx_socket_close(&sock);
  uv_close((uv_handle_t *) &peer, NULL);
}

void
on_send (uv_udp_send_t *req, int status) {
  assert(status == 0);
}

void
send_ack (uv_timer_t *timer) {
  memset(&ack_packet, 0, sizeof(ack_packet));
  ack_packet.magic = UDX_MAGIC_BYTE;
  ack_packet.version = UDX_VERSION;
  ack_packet.remote_id = udx__swap_uint32_if_be(1);
  ack_packet.rwnd = udx__swap_uint32_if_be(UINT32_MAX);
  ack_packet.ack = udx__swap_uint32_if_be(1);

  uv_buf_t buf = uv_buf_init((char *) &ack_packet, sizeof(ack_packet));
  int e = uv_udp_send(&ack_req, &peer, &buf, 1, (struct sockaddr *) &sock_addr, on_send);
  assert(e == 0);

  acked = uv_now(&loop);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 0, &sock_addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &sock_addr, 0);
  assert(e == 0);

  int len = sizeof(sock_addr);
  e = udx_socket_getsockname(&sock, (struct sockaddr *) &sock_addr, &len);
  assert(e == 0);

  e = uv_udp_init(&loop, &peer);
  assert(e == 0);

  struct sockaddr_in peer_addr;
  uv_ip4_addr("127.0.0.1", 0, &peer_addr);
  e = uv_udp_bind(&peer, (struct sockaddr *) &peer_addr, 0);
  assert(e == 0);

  len = sizeof(peer_addr);
  e = uv_udp_getsockname(&peer, (struct sockaddr *) &peer_addr, &len);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &peer_addr);
  assert(e == 0);

  e = udx_stream_set_delivery_timeout(&stream, 3000);
  assert(e == 0);

  size_t data_len = udx__max_payload(&stream) * 2;
  char *data = malloc(data_len);
  memset(data, 'a', data_len);

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  uv_buf_t buf = uv_buf_init(data, data_len);
  e = udx_stream_write(req, &stream, &buf, 1, NULL);
  assert(e != 0);
  assert(stream.seq == 2);

  // without the restart on ack, the stream would time out at 1000 ms
  stream.progress_ts -= 2000;

  uv_timer_init(&loop, &ack_timer);
  uv_timer_start(&ack_timer, send_ack, 100, 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  free(req);
  free(data);

  return 0;
}

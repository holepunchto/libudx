#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// EOF must be delivered once the gap before the peer's END is filled, even when
// the packet that fills it also acks our data and we send more in the same tick.

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_socket_t peer;
udx_stream_t stream;

struct sockaddr_in addr;
struct sockaddr_in peer_addr;

udx_stream_write_t *req;
uv_timer_t timer;

char data[65536];
bool gap_filled = false;
bool eof = false;

static void
peer_send (int type, uint32_t seq, uint32_t ack) {
  struct {
    uint8_t magic, version, type, data_offset;
    uint32_t id, rwnd, seq, ack;
    char data[1];
  } pkt = {0xff, 1, type, 0, udx__swap_uint32_if_be(1), udx__swap_uint32_if_be(0x100000), udx__swap_uint32_if_be(seq), udx__swap_uint32_if_be(ack), {'x'}};

  uv_buf_t buf = uv_buf_init((char *) &pkt, sizeof(pkt));
  int e = uv_udp_try_send(&peer.uv_udp, &buf, 1, (struct sockaddr *) &addr);
  assert(e == sizeof(pkt));
}

static void
on_timeout (uv_timer_t *t) {
  if (!gap_filled) {
    gap_filled = true;
    peer_send(UDX_HEADER_DATA, 0, 2); // fill the gap and ack 2 of our packets
    uv_timer_start(&timer, on_timeout, 500, 0);
    return;
  }

  uv_close((uv_handle_t *) &timer, NULL);
  udx_stream_destroy(&stream);
}

static void
on_peer_recv (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  if (read_len < UDX_HEADER_SIZE || gap_filled || uv_is_active((uv_handle_t *) &timer)) return;
  if (buf->base[2] & UDX_HEADER_SACK) uv_timer_start(&timer, on_timeout, 10, 0); // END was sacked
}

static void
on_read (udx_stream_t *s, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len == UV_EOF) eof = true;
}

static void
on_close (udx_stream_t *s, int status) {
  udx_socket_close(&sock);
  udx_socket_close(&peer);
}

int
main () {
  int e;

  uv_loop_init(&loop);
  uv_timer_init(&loop, &timer);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &peer, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18141, &addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18142, &peer_addr);
  e = udx_socket_bind(&peer, (struct sockaddr *) &peer_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &sock, 2, (struct sockaddr *) &peer_addr);
  assert(e == 0);

  e = udx_stream_read_start(&stream, on_read);
  assert(e == 0);

  e = udx_socket_recv_start(&peer, on_peer_recv);
  assert(e == 0);

  req = malloc(udx_stream_write_sizeof(1));
  uv_buf_t buf = uv_buf_init(data, sizeof(data));
  udx_stream_write(req, &stream, &buf, 1, NULL); // more than the initial window

  peer_send(UDX_HEADER_END, 1, 0); // data seq 0 is "lost"

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  e = uv_loop_close(&loop);
  assert(e == 0);

  free(req);

  assert(eof);

  return 0;
}

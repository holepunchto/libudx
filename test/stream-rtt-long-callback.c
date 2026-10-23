#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// an ack read after a long callback in the same wakeup must not get the time from before
// that callback: the sample comes out shorter than the rtt and pins rtt_min.
// the receiver sends data that the sender reads first. the read callback blocks for 50 ms
// and the ack for P0 arrives 25 ms in, so it is read later in the same wakeup.

#if defined(_WIN32)
void
sleep_ms (long milliseconds) {
  Sleep(milliseconds);
}
#else
#include <time.h>
void
sleep_ms (long milliseconds) {
  struct timespec ts;
  ts.tv_sec = milliseconds / 1000;
  ts.tv_nsec = (milliseconds % 1000) * 1000000L;
  nanosleep(&ts, NULL);
}
#endif

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

udx_stream_write_t *req;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

char data[100];

typedef struct {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t data_offset;
  uint32_t id;
  uint32_t rwnd;
  uint32_t seq;
  uint32_t ack;
  char payload[8];
} packet_t;

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

udx_socket_send_t data_req;
udx_socket_send_t ack_req;

static void
on_sent (udx_socket_send_t *req, int status) {
  (void) req;
  (void) status;
}

static void
send_packet (udx_socket_send_t *req, packet_t *pkt, uint8_t type, uint32_t seq, uint32_t ack, size_t len) {
  memset(pkt, 0, sizeof(*pkt));
  pkt->magic = 0xff;
  pkt->version = 1;
  pkt->type = type;
  pkt->id = udx__swap_uint32_if_be(1);
  pkt->rwnd = 0xffffffffu;
  pkt->seq = udx__swap_uint32_if_be(seq);
  pkt->ack = udx__swap_uint32_if_be(ack);

  uv_buf_t buf = uv_buf_init((char *) pkt, len);
  udx_socket_send(req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_sent);
}

static void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  if (!(buf->base[2] & UDX_HEADER_DATA)) return; // our ack, the destroy packet

  // data that does not ack P0 yet
  static packet_t pkt;
  send_packet(&data_req, &pkt, UDX_HEADER_DATA, 0, 0, sizeof(pkt));
}

static void
on_read (udx_stream_t *s, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len <= 0) return;

  sleep_ms(25);

  // lands on send_sock while libuv is still reading it in this wakeup
  static packet_t pkt;
  send_packet(&ack_req, &pkt, 0, 1, 1, sizeof(pkt) - sizeof(pkt.payload));

  sleep_ms(25);
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);

  printf("rtt-long-callback: srtt=%u rtt_min=%u\n", stream.srtt, udx_rtt_min(&stream));

  // the ack arrived about 25 ms after P0 was sent
  assert(udx_rtt_min(&stream) >= 20);
  assert(stream.srtt >= 20);

  udx_stream_destroy(&stream);
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, on_socket_close);
  assert(e == 0);

  e = udx_socket_init(&udx, &recv_sock, on_socket_close);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18093, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18094, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_stream_read_start(&stream, on_read);
  assert(e == 0);

  e = udx_socket_recv_start(&recv_sock, on_recv);
  assert(e == 0);

  uv_buf_t buf = uv_buf_init(data, sizeof(data));
  udx_stream_write(req, &stream, &buf, 1, on_ack);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  e = uv_loop_close(&loop);
  assert(e == 0);

  free(req);

  return 0;
}

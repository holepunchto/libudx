#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// an ack read after a long callback in the same wakeup must not get the time from before
// that callback: the rtt sample comes out shorter than the rtt and pins rtt_min. the rate
// sample must take the ack's time too, or its interval comes out below rtt_min and it is
// dropped. rack must stay on the loop time, or P1 is not lost on the ack.
// the receiver sends data that the sender reads first. the read callback blocks for 50 ms
// and the ack for P0 with P2-P4 sacked arrives 25 ms in, so it is read later in the same wakeup.

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
uv_timer_t check_timer;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

char data[5000]; // P0-P4

typedef struct {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t data_offset;
  uint32_t id;
  uint32_t rwnd;
  uint32_t seq;
  uint32_t ack;
  uint32_t payload[2]; // the sack block of the ack
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
send_packet (udx_socket_send_t *req, packet_t *pkt, uint8_t type, uint32_t seq, uint32_t ack, uint32_t sack_start, uint32_t sack_end) {
  memset(pkt, 0, sizeof(*pkt));
  pkt->magic = 0xff;
  pkt->version = 1;
  pkt->type = type;
  pkt->id = udx__swap_uint32_if_be(1);
  pkt->rwnd = 0xffffffffu;
  pkt->seq = udx__swap_uint32_if_be(seq);
  pkt->ack = udx__swap_uint32_if_be(ack);
  pkt->payload[0] = udx__swap_uint32_if_be(sack_start);
  pkt->payload[1] = udx__swap_uint32_if_be(sack_end);

  uv_buf_t buf = uv_buf_init((char *) pkt, sizeof(*pkt));
  udx_socket_send(req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_sent);
}

static void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  static bool sent = false;
  if (sent || !(buf->base[2] & UDX_HEADER_DATA)) return;
  sent = true;

  // read in a later millisecond than the send stamps of P0-P4, on the clock of the loop time
  uint64_t until = uv_hrtime() + 2000000;
  while (uv_hrtime() < until) sleep_ms(1);

  // data that does not ack P0 yet
  static packet_t pkt;
  send_packet(&data_req, &pkt, UDX_HEADER_DATA, 0, 0, 0, 0);
}

static void
on_check (uv_timer_t *timer) {
  printf("rtt-long-callback: srtt=%u rtt_min=%u rate_interval_ms=%u lost=%u\n", stream.srtt, udx_rtt_min(&stream), stream.rate_interval_ms, stream.lost);

  // the ack arrived about 25 ms after P0-P4 were sent
  assert(udx_rtt_min(&stream) >= 20);
  assert(stream.srtt >= 20);
  assert(stream.rate_interval_ms >= 20);
  assert(stream.lost == 1);

  uv_close((uv_handle_t *) timer, NULL);
  udx_stream_destroy(&stream);
}

static void
on_read (udx_stream_t *s, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len <= 0) return;

  sleep_ms(25);

  // lands on send_sock while libuv is still reading it in this wakeup
  static packet_t pkt;
  send_packet(&ack_req, &pkt, UDX_HEADER_SACK, 1, 1, 2, 5);

  sleep_ms(25);

  // runs after the rest of this wakeup's reads
  uv_timer_start(&check_timer, on_check, 0, 0);
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  (void) req;
  (void) status;
  (void) unordered;
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

  uv_timer_init(&loop, &check_timer);

  uv_buf_t buf = uv_buf_init(data, sizeof(data));
  udx_stream_write(req, &stream, &buf, 1, on_ack);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  e = uv_loop_close(&loop);
  assert(e == 0);

  free(req);

  return 0;
}

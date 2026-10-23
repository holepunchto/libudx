#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// a loss timer that comes due while a callback blocks the loop must not fire before
// the ack that arrived meanwhile is read: libuv runs due timers before the next poll.
// the receiver acks P1 and then blocks the loop for longer than the probe timeout,
// so the ack is queued on the sender's socket when the TLP timer is due, behind more
// datagrams than libuv reads per wakeup.

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

int transmits[2]; // number of times each packet has been received

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

udx_stream_write_t *req0;
udx_stream_write_t *req1;
uv_timer_t check_timer;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

char data[100];

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

udx_socket_send_t ack_req;

static void
on_ack_sent (udx_socket_send_t *req, int status) {
  (void) req;
  (void) status;
}

static void
send_ack (uint32_t ack) {
  static struct {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t data_offset;
    uint32_t id;
    uint32_t rwnd;
    uint32_t seq;
    uint32_t ack;
  } pkt;

  memset(&pkt, 0, sizeof(pkt));
  pkt.magic = 0xff;
  pkt.version = 1;
  pkt.id = udx__swap_uint32_if_be(1);
  pkt.rwnd = 0xffffffffu;
  pkt.ack = udx__swap_uint32_if_be(ack);

  uv_buf_t buf = uv_buf_init((char *) &pkt, sizeof(pkt));
  udx_socket_send(&ack_req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_ack_sent);
}

static void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  if (!(buf->base[2] & UDX_HEADER_DATA)) return; // the destroy packet

  uint32_t seq = udx__swap_uint32_if_be(*(uint32_t *) (buf->base + 12));

  assert(seq < 2);
  transmits[seq]++;

  if (seq == 0) {
    sleep_ms(20); // the probe timeout (2 * srtt) must outlast the delivery of P1 on slow hosts
    send_ack(1);
    return;
  }

  if (transmits[1] == 1) {
    assert(stream.pending_timer == UDX_TIMER_TLP);

    // non-udx datagrams, dropped by send_sock, ahead of the ack
    static char junk[4];
    uv_buf_t b = uv_buf_init(junk, sizeof(junk));
    for (int i = 0; i < 40; i++) {
      uv_udp_try_send(&recv_sock.uv_udp, &b, 1, (struct sockaddr *) &send_addr);
    }

    send_ack(2);

    // the probe timeout passes before the loop polls again
    sleep_ms(100);
  }
}

static void
on_check (uv_timer_t *timer) {
  printf("tlp-queued-ack: transmits=%d,%d retransmits=%u\n", transmits[0], transmits[1], stream.retransmit_count);

  assert(transmits[0] == 1);
  assert(transmits[1] == 1);
  assert(stream.retransmit_count == 0);

  uv_close((uv_handle_t *) timer, NULL);
  udx_stream_destroy(&stream);
}

static void
on_ack1 (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);

  // let a spurious probe reach the receiver
  uv_timer_start(&check_timer, on_check, 50, 0);
}

static void
on_ack0 (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);

  uv_buf_t buf = uv_buf_init(data, sizeof(data));
  udx_stream_write(req1, &stream, &buf, 1, on_ack1);
}

int
main () {
  int e;

  req0 = malloc(udx_stream_write_sizeof(1));
  req1 = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, on_socket_close);
  assert(e == 0);

  e = udx_socket_init(&udx, &recv_sock, on_socket_close);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18131, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18132, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_socket_recv_start(&recv_sock, on_recv);
  assert(e == 0);

  uv_timer_init(&loop, &check_timer);

  uv_buf_t buf = uv_buf_init(data, sizeof(data));
  udx_stream_write(req0, &stream, &buf, 1, on_ack0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  e = uv_loop_close(&loop);
  assert(e == 0);

  free(req0);
  free(req1);

  return 0;
}

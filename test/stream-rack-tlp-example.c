#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// this test recreates the Fast Recovery scenario from the RACK-TLP RFC section 3.4
// https://datatracker.ietf.org/doc/html/rfc8985#name-an-example-of-rack-tlp-in-a

//  Event  TCP DATA SENDER                            TCP DATA RECEIVER
//  _____  ____________________________________________________________
//    1.   Send P0, P1, P2, P3          -->
//         [P1, P2, P3 dropped by network]
//
//    2.                                <--          Receive P0, ACK P0
//
//    3a.  2RTTs after (2), TLP timer fires *
//    3b.  TLP: retransmits P3          -->
//
//    4.                                <--         Receive P3, SACK P3
//
//    5a.  Receive SACK for P3 **
//    5b.  RACK: marks P1, P2 lost
//    5c.  Retransmit P1, P2            -->
//         [P1 retransmission dropped by network]
//
//    6.                                <--    Receive P2, SACK P2 & P3
//
//    7a.  RACK: marks P1 retransmission lost
//    7b.  Retransmit P1                -->
//
//    8.                                <--          Receive P1, ACK P3
//
// * if the P0 ACK has 0rtt then the TLP will be scheduled for 1 second
// instead of 2*srtt
// ** if the rtt is < minimum_windowed_rtt then the the rtt info and rack_time_next might
//    be ignored, in this case an RTO would be triggered instead of Fast Recovery

// we need to sleep for a few milliseconds when sending our ACK
// packet, or we will get a 0 rtt and be unable to disambiguate the ACK for the
// TLP (which is a retransmit of packet 3) from an ACK of the original
// transmission, thus being unable to use it to set rack_time_sent
// and rack_next_seq, and preventing the fast recovery it is
// meant to trigger in step 5a, 5b, and 5c.
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

int event = 1;
int transmits[4]; // number of times each packet has been received

uint64_t t0;

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

uv_timer_t sack_timer;
uv_timer_t check_timer;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

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

void
on_ack_sent (udx_socket_send_t *req, int status) {
  (void) req;
  (void) status;
}

static void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {

  if (!t0) {
    t0 = uv_now(&loop);
  }

  int time_ms = uv_now(&loop) - t0;

  uint8_t *b = (uint8_t *) buf->base;

  uint8_t magic = *b++;
  uint8_t version = *b++;
  uint8_t type = *b++;
  uint8_t data_offset = *b++;
  uint32_t *i = (uint32_t *) b;

  uint32_t id = *i++;
  uint32_t rwnd = *i++;
  uint32_t seq = *i++;
  uint32_t ack = *i++;
  printf("rack-tlp-test: on_recv time=%d seq=%u len=%d\n", time_ms, seq, (int) buf->len);

  transmits[seq]++;

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
  pkt.type = 0;
  pkt.data_offset = 0;
  pkt.id = udx__swap_uint32_if_be(1);
  pkt.rwnd = 0xffffffffu;

  if (event == 1) {
    if (transmits[0] == 1 &&
        transmits[1] == 1 &&
        transmits[2] == 1 &&
        transmits[3] == 1) {
      // once we've received all 4 packets, pretend to
      // have lost 1,2,3 and ack p0
      pkt.ack = udx__swap_uint32_if_be(1);
      assert(stream.seq == 4);
      assert(stream.remote_acked == 0);
      assert(stream.pending_timer == UDX_TIMER_TLP);

      uv_buf_t buf = uv_buf_init((char *) &pkt, sizeof(pkt));
      udx_socket_send(&ack_req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_ack_sent);
      printf("rack-tlp-test: event=2 time=%u Receive P0, ACK P0\n", time_ms);
      event = 2;
    }
    return;
  }

  // the scenario hops from events 1-8, but since we only implement the receiver side
  // for the test we hop from events 1-2-4-6-8

  else if (event == 2) {
    assert(seq == 3);
    assert(transmits[0] == 1);
    assert(transmits[1] == 1);
    assert(transmits[2] == 1);
    assert(transmits[3] == 2); // only TLP was retransmitted

    assert(stream.pending_timer == UDX_TIMER_RTO); // since we've receive the TLP here the RTO should be set
    assert(stream.rack_fack == 1);
    // we've received the tlp for packet 3
    pkt.ack = udx__swap_uint32_if_be(1);
    pkt.type |= UDX_HEADER_SACK;
    pkt.sack.start = udx__swap_uint32_if_be(3);
    pkt.sack.end = udx__swap_uint32_if_be(4);

    sleep_ms(2);

    uv_buf_t buf = uv_buf_init((char *) &pkt, sizeof(pkt));
    udx_socket_send(&ack_req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_ack_sent);
    printf("rack-tlp-test: event=4 time=%u Receive P3, SACK P3\n", time_ms);
    event = 4;
    return;
  }

  else if (event == 4) {
    assert(seq == 1 || seq == 2);
    if (seq == 1) {
      assert(transmits[1] == 2);
      printf("rack-tlp-test: event=5c, time=%u dropping P1\n", time_ms);
    }
    if (seq == 2) {
      assert(transmits[2] == 2);
    }
    if (transmits[1] == 2 && transmits[2] == 2) {
      // pretend to have dropped retransmit of 1 and SACK 2:4
      // probably RTO is set, but if there is re-ordering it is possible that RACK_REO is set
      assert(stream.pending_timer == UDX_TIMER_RTO || stream.pending_timer == UDX_TIMER_RACK_REO);
      assert(stream.rack_fack == 4);
      pkt.ack = udx__swap_uint32_if_be(1);
      pkt.type |= UDX_HEADER_SACK;
      pkt.sack.start = udx__swap_uint32_if_be(2);
      pkt.sack.end = udx__swap_uint32_if_be(4);

      sleep_ms(2);
      uv_buf_t buf = uv_buf_init((char *) &pkt, sizeof(pkt));
      udx_socket_send(&ack_req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_ack_sent);
      printf("rack-tlp-test: event=6 time=%u Receive P2, SACK P2 & P3\n", time_ms);

      event = 6;
    }
    return;
  }

  else if (event == 6) {
    assert(seq == 1);
    assert(transmits[1] == 3);

    pkt.ack = 4;

    sleep_ms(2);
    uv_buf_t buf = uv_buf_init((char *) &pkt, sizeof(pkt));
    udx_socket_send(&ack_req, &recv_sock, &buf, 1, (struct sockaddr *) &send_addr, on_ack_sent);

    printf("rack-tlp-test: event=8 time=%u Receive P1, ACK P3\n", time_ms);

    event = 8;
    return;
  }
  printf("rack-tlp-test: event=%d time=%u unexpected packet seq=%u\n", event, time_ms, seq);
  assert(false && "receive unexpected packet!");
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(stream.rto_count == 0);
  assert(stream.fast_recovery_count == 1);
  assert(transmits[0] == 1);
  assert(transmits[1] == 3);
  assert(transmits[2] == 2);
  assert(transmits[3] == 2);

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

  e = udx_socket_recv_start(&recv_sock, on_recv);

  int payload_size = 4400; // will generate 4 packets

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

  return 0;
}

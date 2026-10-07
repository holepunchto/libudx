#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// timewait state
// 1. sender (real) send stream writes END packet. since it has not received an END already it is the active closer.
//    receiver (emulated) stream replies END packet and ACKs END packet. the sender will enter TIME-WAIT
// 2. receiver (emulated) stream receives ACK of END packet, but pretends it didn't receive it.
//    receiver (emulated) stream retransmits END+ACK packet
// 3. the real stream, which is in TIME-WAIT, retransmits the final ACK packet
//    receiver (emulated) receives the retransmit of the final ACK

uint64_t t0;
int event;

bool stream_closed;
bool write_acked;
bool ack_retransmitted;

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

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

  printf("stream closed status=%X\n", status);

  stream_closed = true;
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
  printf("timewait test: event=%d time=%d seq=%u ack=%u type=%x len=%d stream.status=%x\n", event, time_ms, seq, ack, type, (int) buf->len, stream.status);

  struct {
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
  pkt.type = 0;
  pkt.data_offset = 0;
  pkt.id = udx__swap_uint32_if_be(1);
  pkt.rwnd = 0xffffffffu;

  uv_buf_t reply_buf;

  switch (event) {
  case 0:
    // we receive an END packet. send our own ACK + END packet
    assert(stream.status & UDX_STREAM_TIMEWAIT_WANTED);
    pkt.type |= UDX_HEADER_END;
    pkt.ack = udx__swap_uint32_if_be(1);
    reply_buf = uv_buf_init((char *) &pkt, sizeof(pkt));
    udx_socket_send(&ack_req, &recv_sock, &reply_buf, 1, (struct sockaddr *) &send_addr, on_ack_sent);

    break;

  case 1:
    assert(write_acked);
    assert(stream_closed);
    // we receive an ACK to our end packet
    // instead of ending we retransmit the END packet
    pkt.type |= UDX_HEADER_END;
    pkt.ack = udx__swap_uint32_if_be(1);
    reply_buf = uv_buf_init((char *) &pkt, sizeof(pkt));
    udx_socket_send(&ack_req, &recv_sock, &reply_buf, 1, (struct sockaddr *) &send_addr, on_ack_sent);
    break;

  case 2:
    // the remote in time-wait state retransmits the final ack
    printf("we receive another ACK from the remote in time-wait state\n");

    ack_retransmitted = true;

    udx_socket_close(&send_sock);
    udx_socket_close(&recv_sock);

    break;

  default:
    assert(false);
  }

  event++;
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  write_acked = true;
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

  e = udx_stream_write_end(req, &stream, NULL, 0, on_ack);
  assert(e);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  assert(ack_retransmitted);

  e = uv_loop_close(&loop);
  assert(e == 0);

  free(req);

  return 0;
}

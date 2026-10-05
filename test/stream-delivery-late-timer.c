#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

// block the loop for longer than the delivery timeout while data is unacked,
// the streams must not time out and the data must arrive after the stall
// a: rtt known, written right before the stall, so the late timer is the tlp,
//    its receiver appears just after the stall
// b: no rtt, written at start and retransmitted once, so the late timer is the
//    rto, its receiver appears right before the stall

#define TIMEOUT 3000
#define STALL   4500

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

udx_stream_t a_send;
udx_stream_t a_recv;
udx_stream_t b_send;
udx_stream_t b_recv;

uv_timer_t stall_timer;
uv_timer_t recv_timer;

int acked = 0;
int closed = 0;

void
on_close (udx_stream_t *stream, int status) {
  assert(status == 0);

  if (++closed == 4) {
    udx_socket_close(&send_sock);
    udx_socket_close(&recv_sock);
  }
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  free(req);

  if (++acked == 2) {
    udx_stream_destroy(&a_send);
    udx_stream_destroy(&a_recv);
    udx_stream_destroy(&b_send);
    udx_stream_destroy(&b_recv);
  }
}

void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {}

void
write_hello (udx_stream_t *stream) {
  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  uv_buf_t buf = uv_buf_init("hello", 5);
  int e = udx_stream_write(req, stream, &buf, 1, on_ack);
  assert(e);
}

void
start_receiver (udx_stream_t *stream, uint32_t local_id) {
  int e = udx_stream_init(&udx, stream, local_id, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(stream, &recv_sock, local_id - 1, (struct sockaddr *) &send_addr);
  assert(e == 0);

  e = udx_stream_read_start(stream, on_read);
  assert(e == 0);
}

void
on_recv_timer (uv_timer_t *timer) {
  start_receiver(&a_recv, 2);
}

void
on_stall (uv_timer_t *timer) {
  write_hello(&a_send);
  start_receiver(&b_recv, 4);

  // loop time is not updated during the stall, so this fires 20 ms after it
  uv_timer_start(&recv_timer, on_recv_timer, STALL + 20, 0);

  uv_sleep(STALL);
}

void
connect_sender (udx_stream_t *stream, uint32_t local_id) {
  int e = udx_stream_init(&udx, stream, local_id, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(stream, &send_sock, local_id + 1, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_stream_set_delivery_timeout(stream, TIMEOUT);
  assert(e == 0);
}

void
bind_any (udx_socket_t *socket, struct sockaddr_in *addr) {
  int e = udx_socket_init(&udx, socket, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 0, addr);
  e = udx_socket_bind(socket, (struct sockaddr *) addr, 0);
  assert(e == 0);

  int len = sizeof(*addr);
  e = udx_socket_getsockname(socket, (struct sockaddr *) addr, &len);
  assert(e == 0);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  bind_any(&send_sock, &send_addr);
  bind_any(&recv_sock, &recv_addr);

  connect_sender(&a_send, 1);
  connect_sender(&b_send, 3);

  // pretend a has an rtt estimate, so a tlp is armed
  a_send.srtt = 5;
  a_send.rttvar = 2;
  a_send.tlp_permitted = true;

  write_hello(&b_send);

  uv_timer_init(&loop, &stall_timer);
  uv_timer_init(&loop, &recv_timer);
  uv_timer_start(&stall_timer, on_stall, 1500, 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  assert(acked == 2);
  assert(closed == 4);

  return 0;
}

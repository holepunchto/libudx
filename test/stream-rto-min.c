// Keep the regression checks active in Release builds too.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;
udx_socket_t sock;
udx_stream_t sender;
udx_stream_t receiver;
uv_timer_t watchdog;
int writes_acked;

static void
on_write_acked (udx_stream_write_t *req, int status, int unordered) {
  (void) req;
  assert(status == 0);
  assert(unordered == 0);
  writes_acked++;
}

static void
on_timeout (uv_timer_t *timer) {
  (void) timer;
  fputs("RTO minimum test timed out\n", stderr);
  abort();
}

int
main () {
  int err = uv_loop_init(&loop);
  assert(err == 0);
  err = udx_init(&loop, &udx, NULL);
  assert(err == 0);
  err = udx_socket_init(&udx, &sock, NULL);
  assert(err == 0);

  struct sockaddr_in addr;
  err = uv_ip4_addr("127.0.0.1", 0, &addr);
  assert(err == 0);
  err = udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);
  assert(err == 0);
  int addr_len = sizeof(addr);
  err = udx_socket_getsockname(&sock, (struct sockaddr *) &addr, &addr_len);
  assert(err == 0);

  err = udx_stream_init(&udx, &sender, 1, NULL, NULL);
  assert(err == 0);
  err = udx_stream_init(&udx, &receiver, 2, NULL, NULL);
  assert(err == 0);
  err = udx_stream_connect(&sender, &sock, 2, (struct sockaddr *) &addr);
  assert(err == 0);
  err = udx_stream_connect(&receiver, &sock, 1, (struct sockaddr *) &addr);
  assert(err == 0);
  assert(sender.rto == 1000);

  err = uv_timer_init(&loop, &watchdog);
  assert(err == 0);
  err = uv_timer_start(&watchdog, on_timeout, 10000, 0);
  assert(err == 0);

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  assert(req != NULL);
  uv_buf_t buf = uv_buf_init("x", 1);
  uint32_t estimated_rto;
  // Allow a slow first sample to settle, but require exercising the floor.
  for (int attempt = 0; attempt < 32; attempt++) {
    int ack_target = writes_acked + 1;
    err = udx_stream_write(req, &sender, &buf, 1, on_write_acked);
    assert(err > 0);
    while (writes_acked < ack_target) uv_run(&loop, UV_RUN_ONCE);
    estimated_rto = sender.srtt + 4 * sender.rttvar;
    assert(sender.rto == (estimated_rto < 200 ? 200 : estimated_rto));
    if (estimated_rto < 200) break;
  }
  assert(estimated_rto < 200);
  assert(sender.rto == 200);

  // A slower, more variable path must retain its larger calculated timeout.
  sender.srtt = 400;
  sender.rttvar = 100;
  int ack_target = writes_acked + 1;
  err = udx_stream_write(req, &sender, &buf, 1, on_write_acked);
  assert(err > 0);
  while (writes_acked < ack_target) uv_run(&loop, UV_RUN_ONCE);
  estimated_rto = sender.srtt + 4 * sender.rttvar;
  assert(estimated_rto > 200);
  assert(sender.rto == estimated_rto);

  uv_close((uv_handle_t *) &watchdog, NULL);
  err = udx_stream_destroy(&sender);
  assert(err >= 0);
  err = udx_stream_destroy(&receiver);
  assert(err >= 0);
  err = udx_socket_close(&sock);
  assert(err == 0);
  err = uv_run(&loop, UV_RUN_DEFAULT);
  assert(err == 0);
  err = uv_loop_close(&loop);
  assert(err == 0);

  free(req);
  return 0;
}

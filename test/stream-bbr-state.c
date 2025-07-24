

// how bbr works:
// 1. compute a rate sample at each ack, or a dummy sample on a loss event
// 2. update the model with the rate sample
//   a. bbr_update bw
//     i. divides time into rounds, where a round starts is determined
//       by if rs.delivered >= next_rtt_delivered
//   b. bbr_update_ack_aggregation
//
// 3. set pacing rate and cwnd based on the model

// goal: test states of bbr
// 1. test STARTUP phase with scarce data
//   a. expected result: should remain in STARTUP
// 2. send mass data until we leave STARTUP and enter steady state
//   a. expected result: we enter DRAIN state
//   b. todo: check that we're guaranteed an ACK in DRAIN state
//     i. should be the case - state can only advance in response to an ACK after all
// 3. continue sending at max rate once we enter steady state (PROBE_BW / PROBE_RTT)
//   a. install an on_ack hook
//   b. wait for at least 8 cycles + rtt measurement of time
//     i. cycle length is stream->bbr.min_rtt_ms
//   c. confirm we've received in ack in each of the gain phases and at least one in PROBE_RTT phase
//   d. stop sending rapidly and switch to app limited behavior
// 4. switch back to application limited sending
//   a. confirm that our next ack reverts the stream to app limited

#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "helpers.h"

uv_loop_t loop;
udx_t udx;
uint64_t start_ms;
uv_timer_t timer;

udx_socket_t send_sock;
udx_stream_t send_stream;

udx_socket_t recv_sock;
udx_stream_t recv_stream;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

#define STATE_LOW_BW   1
#define STATE_HIGH_BW  2
#define STATE_FINISHED 3

// spend 5 seconds sending a once-per-second heartbeat
// then  5 seconds sending at maximum throughput
// then  5 seconds sending a once-per-second heartbeat
// then  5 seconds sending at maximum throughput

int states[][2] = {
  {5000, STATE_LOW_BW}, /* { duration, state } */
  {5000, STATE_HIGH_BW},
  {5000, STATE_LOW_BW},
  {5000, STATE_HIGH_BW}
  // conceptually state[4] = { infinity, STATE_FINISHED }
};

int state; // index into state table, only increases

static int
state_from_time (uint64_t now) {
  int64_t elapsed = now - start_ms;
  int64_t next_state_start_time_ms = 0;

  for (int i = 0; i < (sizeof(states) / sizeof(states[0])); i++) {
    next_state_start_time_ms += states[i][0];
    if (elapsed < next_state_start_time_ms) {
      return states[i][1];
    }
  }

  return STATE_FINISHED;
}

unsigned char small_message[] = "hello";
unsigned char large_message[0x1000];

static void
send_ack_slow (udx_stream_write_t *req, int status, int unordered);

static void
send_ack_fast (udx_stream_write_t *req, int status, int unordered);

static void
pump_writes (udx_stream_t *stream) {
  while (state == STATE_HIGH_BW) {
    uv_buf_t large_buf = uv_buf_init((char *) &large_message, sizeof(large_message));
    udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

    int rc = udx_stream_write(req, stream, &large_buf, 1, send_ack_fast);
    assert(rc >= 0);

    if (rc == 0) { // high watermark
      break;
    }
    udx_stream_write_resume(stream, pump_writes);
  }
}

udx_stream_write_t *send_end_request;
udx_stream_write_t *recv_end_request;

static void
send_periodic (uv_timer_t *timer) {
  int prev_state = state;
  state = state_from_time(uv_now(timer->loop));

  char *state_names[] = {
    "STATE_INIT",
    "STATE_LOW_BW",
    "STATE_HIGH_BW",
    "STATE_FINISHED",
  };

  if (prev_state != state) {
    printf("test stream-bbr-state: driver state changed from %s -> %s\n", state_names[prev_state], state_names[state]);

    if (state == STATE_HIGH_BW) {
      // transitioned into high bw
      pump_writes(&send_stream);
    }
  }

  if (state == STATE_LOW_BW) {
    uv_buf_t small_buf = uv_buf_init((char *) small_message, sizeof(small_message));
    udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

    int rc = udx_stream_write(req, &send_stream, &small_buf, 1, send_ack_slow);
    assert(rc >= 0);
  }

  // printf("send rate=%9.3fpkts/msec %s\n", send_stream.rate_delivered / (1.0f * send_stream.rate_interval_ms), send_stream.rate_sample_is_app_limited ? "(app limited)" : "");

  char *bbr_state_name[] = {
    "UDX_BBR_STATE_STARTUP",
    "UDX_BBR_STATE_DRAIN",
    "UDX_BBR_STATE_PROBE_BW",
    "UDX_BBR_STATE_PROBE_RTT"
  };

  printf("bbr.state=%s\n", bbr_state_name[send_stream.bbr.state]);

  if (state == STATE_FINISHED) {
    int rc;
    send_end_request = malloc(udx_stream_write_sizeof(1));
    recv_end_request = malloc(udx_stream_write_sizeof(1));
    rc = udx_stream_write_end(send_end_request, &send_stream, NULL, 0, NULL);
    assert(rc >= 0);
    rc = udx_stream_write_end(recv_end_request, &recv_stream, NULL, 0, NULL);
    assert(rc >= 0);
    uv_timer_stop(timer);
  }
}

static void
send_ack_slow (udx_stream_write_t *r, int status, int unordered) {
  // printf("send_ack (slow)\n");
}

static void
send_ack_fast (udx_stream_write_t *r, int status, int unordered) {
  // printf("send_ack (fast) elapsed=%ld\n", uv_now(&loop) - start_ms);
}

static void
recv_read (udx_stream_t *recv, ssize_t read_len, const uv_buf_t *buf) {
  // printf("on_read\n");
}

// other callbacks
static void
sock_close (udx_socket_t *sock) {
  printf("sock close\n");
}

int nclosed;

static void
stream_close (udx_stream_t *stream, int status) {
  nclosed++;
  if (nclosed == 2) {
    assert(udx_socket_close(&send_sock) == 0);
    assert(udx_socket_close(&recv_sock) == 0);
  }
}

int
main () {

  int rc = 0;

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  rc = udx_init(&loop, &udx, NULL);
  assert(rc == 0);

  rc = udx_socket_init(&udx, &send_sock, sock_close);
  assert(rc == 0);

  rc = udx_socket_init(&udx, &recv_sock, sock_close);
  assert(rc == 0);

  uv_ip4_addr("127.0.0.1", 8082, &send_addr);
  rc = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(rc == 0);

  uv_ip4_addr("127.0.0.1", 8081, &recv_addr);
  rc = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(rc == 0);

  // stream 1 -> stream 2

  rc = udx_stream_init(&udx, &send_stream, 1, stream_close, NULL);
  assert(rc == 0);

  rc = udx_stream_init(&udx, &recv_stream, 2, stream_close, NULL);
  assert(rc == 0);

  rc = udx_stream_connect(&send_stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(rc == 0);

  rc = udx_stream_connect(&recv_stream, &recv_sock, 1, (struct sockaddr *) &send_addr);
  assert(rc == 0);

  rc = udx_stream_read_start(&recv_stream, recv_read);
  assert(rc == 0);

  uv_timer_init(&loop, &timer);
  start_ms = uv_now(&loop);

  uv_timer_start(&timer, send_periodic, 0, 1000);

  rc = uv_run(&loop, UV_RUN_DEFAULT);
  assert(rc == 0);

  uv_loop_close(&loop);
}

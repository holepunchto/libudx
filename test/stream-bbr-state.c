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
// 1. send a heartbeat every DRIVER_INTERVAL_MS for LOW_BW_MS
//   a. expected result: should remain in STARTUP
// 2. send at max rate until bbr has been through PROBE_RTT and back to PROBE_BW
//   a. expected result: we leave STARTUP into PROBE_BW and see each of its gain phases. DRAIN isn't
//      checked: one ack can enter and leave it (bbr_check_drain), so it may not be seen
//   b. PROBE_RTT starts when the min_rtt sample is UDX_BBR_MIN_RTT_INTERVAL_MS (10s) old. rather than
//      wait, once every gain phase has been seen, the sample's timestamp is moved back so the next
//      ack finds it expired
//   c. bbr's state is checked after each loop iteration's packets are processed
// 3. switch back to application limited sending once the fast writes are acked
//   a. expected result: the heartbeat's rate sample is app limited. a sample that isn't is always
//      saved in stream->rate_*, an app limited one only if it's faster than the saved one
//   b. expected result: max_bw is unchanged

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
uv_check_t observer;

udx_socket_t send_sock;
udx_stream_t send_stream;

udx_socket_t recv_sock;
udx_stream_t recv_stream;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

#define STATE_INIT        0
#define STATE_LOW_BW      1
#define STATE_HIGH_BW     2
#define STATE_APP_LIMITED 3
#define STATE_FINISHED    4

#define DRIVER_INTERVAL_MS 250
#define LOW_BW_MS          500
#define TIMEOUT_MS         5000 // less than MIN_RTT_INTERVAL_MS, so PROBE_RTT can't come from waiting

#define MIN_RTT_INTERVAL_MS 10000 // UDX_BBR_MIN_RTT_INTERVAL_MS in src/udx_bbr.c

int state; // only increases

char *state_names[] = {
  "STATE_INIT",
  "STATE_LOW_BW",
  "STATE_HIGH_BW",
  "STATE_APP_LIMITED",
  "STATE_FINISHED",
};

char *bbr_state_names[] = {
  "UDX_BBR_STATE_STARTUP",
  "UDX_BBR_STATE_DRAIN",
  "UDX_BBR_STATE_PROBE_BW",
  "UDX_BBR_STATE_PROBE_RTT"
};

uint32_t bbr_states_seen;    // bit per UDX_BBR_STATE_*, while in STATE_HIGH_BW
uint32_t cycle_indexes_seen; // bit per PROBE_BW gain phase, while in STATE_HIGH_BW
bool min_rtt_expired;
int fast_writes_pending;
bool heartbeat_sent;
double max_bw_before_heartbeat;
uint32_t rate_delivered_before_heartbeat;
uint32_t rate_interval_before_heartbeat;

unsigned char small_message[] = "hello";
unsigned char large_message[0x1000];

udx_stream_write_t *send_end_request;
udx_stream_write_t *recv_end_request;

static uint64_t
elapsed_ms (void) {
  return uv_now(&loop) - start_ms;
}

static void
set_state (int next) {
  printf("test stream-bbr-state: t=%" PRIu64 "ms driver state changed from %s -> %s\n", elapsed_ms(), state_names[state], state_names[next]);
  state = next;
}

static double
max_bw (void) {
  return send_stream.bbr.bw.entries[0].v;
}

static void
send_ack_slow (udx_stream_write_t *req, int status, int unordered);

static void
send_ack_fast (udx_stream_write_t *req, int status, int unordered);

static void
send_ack_app_limited (udx_stream_write_t *req, int status, int unordered);

static void
pump_writes (udx_stream_t *stream) {
  while (state == STATE_HIGH_BW) {
    uv_buf_t large_buf = uv_buf_init((char *) &large_message, sizeof(large_message));
    udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

    int rc = udx_stream_write(req, stream, &large_buf, 1, send_ack_fast);
    assert(rc >= 0);
    fast_writes_pending++;

    if (rc == 0) { // high watermark
      udx_stream_write_resume(stream, pump_writes);
      break;
    }
  }
}

static void
write_heartbeat (udx_stream_ack_cb cb) {
  uv_buf_t small_buf = uv_buf_init((char *) small_message, sizeof(small_message));
  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

  int rc = udx_stream_write(req, &send_stream, &small_buf, 1, cb);
  assert(rc >= 0);
}

static void
finish (void) {
  set_state(STATE_FINISHED);

  int rc;
  send_end_request = malloc(udx_stream_write_sizeof(1));
  recv_end_request = malloc(udx_stream_write_sizeof(1));
  rc = udx_stream_write_end(send_end_request, &send_stream, NULL, 0, NULL);
  assert(rc >= 0);
  rc = udx_stream_write_end(recv_end_request, &recv_stream, NULL, 0, NULL);
  assert(rc >= 0);
  uv_timer_stop(&timer);
  uv_close((uv_handle_t *) &timer, NULL);
  uv_close((uv_handle_t *) &observer, NULL);
}

// runs after each loop iteration's packets are processed
static void
observe_bbr (uv_check_t *check) {
  if (state != STATE_HIGH_BW) return;

  int bbr_state = send_stream.bbr.state;

  if (!(bbr_states_seen & (1u << bbr_state))) {
    printf("test stream-bbr-state: t=%" PRIu64 "ms bbr.state=%s\n", elapsed_ms(), bbr_state_names[bbr_state]);
  }

  bbr_states_seen |= 1u << bbr_state;

  if (bbr_state == UDX_BBR_STATE_PROBE_BW) {
    cycle_indexes_seen |= 1u << send_stream.bbr.cycle_index;

    // every gain phase seen, expire the min_rtt sample to start PROBE_RTT
    if (cycle_indexes_seen == 0xff && !min_rtt_expired) {
      printf("test stream-bbr-state: t=%" PRIu64 "ms expiring min_rtt\n", elapsed_ms());
      send_stream.bbr.min_rtt_stamp = uv_now(&loop) - MIN_RTT_INTERVAL_MS - 1;
      min_rtt_expired = true;
    }

    // back from PROBE_RTT, stop sending rapidly
    if (bbr_states_seen & (1u << UDX_BBR_STATE_PROBE_RTT)) {
      set_state(STATE_APP_LIMITED);
    }
  }
}

static void
on_driver_timer (uv_timer_t *timer) {
  uint64_t elapsed = elapsed_ms();

  if (elapsed >= TIMEOUT_MS) {
    printf("test stream-bbr-state: timed out in %s, bbr states seen=0x%x\n", state_names[state], bbr_states_seen);
    assert(false);
  }

  if (state == STATE_INIT) {
    set_state(STATE_LOW_BW);
  }

  if (state == STATE_LOW_BW && elapsed >= LOW_BW_MS) {
    set_state(STATE_HIGH_BW);
    pump_writes(&send_stream);
    return;
  }

  if (state == STATE_LOW_BW) {
    printf("test stream-bbr-state: bbr.state=%s\n", bbr_state_names[send_stream.bbr.state]);
    assert(send_stream.bbr.state == UDX_BBR_STATE_STARTUP);
    write_heartbeat(send_ack_slow);
  }

  // wait for the fast writes to be acked so the heartbeat is app limited
  if (state == STATE_APP_LIMITED && !heartbeat_sent && fast_writes_pending == 0) {
    assert(send_stream.bbr.full_bw_reached);
    assert(bbr_states_seen & (1u << UDX_BBR_STATE_STARTUP));
    assert(bbr_states_seen & (1u << UDX_BBR_STATE_PROBE_BW));
    assert(bbr_states_seen & (1u << UDX_BBR_STATE_PROBE_RTT));
    assert(cycle_indexes_seen == 0xff);

    heartbeat_sent = true;
    max_bw_before_heartbeat = max_bw();
    rate_delivered_before_heartbeat = send_stream.rate_delivered;
    rate_interval_before_heartbeat = send_stream.rate_interval_ms;
    write_heartbeat(send_ack_app_limited);
  }
}

static void
send_ack_slow (udx_stream_write_t *r, int status, int unordered) {
  free(r);
}

static void
send_ack_fast (udx_stream_write_t *r, int status, int unordered) {
  fast_writes_pending--;
  free(r);
}

static void
send_ack_app_limited (udx_stream_write_t *r, int status, int unordered) {
  printf("test stream-bbr-state: app limited ack, max_bw before=%.3f after=%.3f pkts/ms\n", max_bw_before_heartbeat, max_bw());
  printf("test stream-bbr-state: app limited ack, rate sample before=%u/%ums after=%u/%ums app_limited=%d\n", rate_delivered_before_heartbeat, rate_interval_before_heartbeat, send_stream.rate_delivered, send_stream.rate_interval_ms, send_stream.rate_sample_is_app_limited);
  assert(send_stream.rate_sample_is_app_limited || (send_stream.rate_delivered == rate_delivered_before_heartbeat && send_stream.rate_interval_ms == rate_interval_before_heartbeat));
  assert(max_bw() == max_bw_before_heartbeat);
  free(r);
  finish();
}

static void
recv_read (udx_stream_t *recv, ssize_t read_len, const uv_buf_t *buf) {
}

static void
sock_close (udx_socket_t *sock) {
  printf("sock close\n");
}

int nclosed;

static void
stream_close (udx_stream_t *stream, int status) {
  nclosed++;
  if (nclosed == 2) {
    int rc;
    rc = udx_socket_close(&send_sock);
    assert(rc == 0);
    rc = udx_socket_close(&recv_sock);
    assert(rc == 0);
  }
}

int
main () {

  int rc = 0;

  uv_loop_init(&loop);

  rc = udx_init(&loop, &udx, NULL);
  assert(rc == 0);

  rc = udx_socket_init(&udx, &send_sock, sock_close);
  assert(rc == 0);

  rc = udx_socket_init(&udx, &recv_sock, sock_close);
  assert(rc == 0);

  uv_ip4_addr("127.0.0.1", 0, &send_addr);
  rc = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(rc == 0);

  int send_addr_len = sizeof(send_addr);
  rc = udx_socket_getsockname(&send_sock, (struct sockaddr *) &send_addr, &send_addr_len);
  assert(rc == 0);

  uv_ip4_addr("127.0.0.1", 0, &recv_addr);
  rc = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(rc == 0);

  int recv_addr_len = sizeof(recv_addr);
  rc = udx_socket_getsockname(&recv_sock, (struct sockaddr *) &recv_addr, &recv_addr_len);
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

  uv_check_init(&loop, &observer);
  uv_check_start(&observer, observe_bbr);

  uv_timer_init(&loop, &timer);
  start_ms = uv_now(&loop);

  uv_timer_start(&timer, on_driver_timer, 0, DRIVER_INTERVAL_MS);

  rc = uv_run(&loop, UV_RUN_DEFAULT);
  assert(rc == 0);

  uv_loop_close(&loop);
}

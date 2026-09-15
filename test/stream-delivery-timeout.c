// Keep the regression checks active in Release builds too.
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/endian.h"

typedef struct {
  uint8_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t data_offset;
  uint32_t id;
  uint32_t rwnd;
  uint32_t seq;
  uint32_t ack;
  uint32_t sack_start;
  uint32_t sack_end;
} ack_packet_t;

typedef struct {
  uv_loop_t loop;
  udx_t udx;
  udx_socket_t sender;
  udx_socket_t receiver;
  udx_stream_t stream;
  uv_timer_t watchdog;
  uv_timer_t step_timer;
  struct sockaddr_in sender_addr;
  struct sockaddr_in receiver_addr;
  udx_stream_write_t *writes[4];
  char payloads[4];
  udx_socket_send_t ack_req;
  ack_packet_t ack_packet;
  unsigned int writes_started;
  unsigned int writes_acked;
  unsigned int writes_cancelled;
  unsigned int unordered_acks;
  unsigned int transmits[4];
  int expected_close_status;
  bool ack_sent;
  bool step_done;
  bool closed;
  bool finalized;
} fixture_t;

static void
on_socket_close (udx_socket_t *socket) {
  (void) socket;
}

static void
on_stream_finalize (udx_stream_t *stream) {
  fixture_t *f = stream->data;
  assert(f->closed);
  f->finalized = true;
}

static void
on_stream_close (udx_stream_t *stream, int status) {
  fixture_t *f = stream->data;
  assert(!f->closed);
  assert(status == f->expected_close_status);
  f->closed = true;

  // A setter must never try to restart a handle belonging to a closed stream.
  int err = udx_stream_set_delivery_timeout(stream, 100);
  assert(err == UV_EINVAL);

  uv_timer_stop(&f->watchdog);
  uv_timer_stop(&f->step_timer);
  uv_close((uv_handle_t *) &f->watchdog, NULL);
  uv_close((uv_handle_t *) &f->step_timer, NULL);
  err = udx_socket_close(&f->sender);
  assert(err == 0);
  err = udx_socket_close(&f->receiver);
  assert(err == 0);
}

static void
on_write_acked (udx_stream_write_t *req, int status, int unordered) {
  fixture_t *f = req->stream->data;
  if (status == UV_ECANCELED) {
    assert(f->closed);
    f->writes_cancelled++;
  } else {
    assert(status == 0);
    f->writes_acked++;
    if (unordered) f->unordered_acks++;
  }
}

static void
on_receiver_read (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  fixture_t *f = socket->data;
  (void) from;
  assert(read_len >= UDX_HEADER_SIZE);

  ack_packet_t header;
  memcpy(&header, buf->base, UDX_HEADER_SIZE);
  if (!(header.type & (UDX_HEADER_DATA | UDX_HEADER_END))) return;

  uint32_t seq = udx__swap_uint32_if_be(header.seq);
  assert(seq < 4);
  f->transmits[seq]++;
}

static void
on_watchdog (uv_timer_t *timer) {
  (void) timer;
  fputs("Delivery timeout test timed out\n", stderr);
  abort();
}

static void
on_step (uv_timer_t *timer) {
  fixture_t *f = timer->data;
  f->step_done = true;
}

static void
wait_ms (fixture_t *f, uint64_t timeout) {
  f->step_done = false;
  int err = uv_timer_start(&f->step_timer, on_step, timeout, 0);
  assert(err == 0);
  while (!f->step_done) {
    uv_run(&f->loop, UV_RUN_ONCE);
    assert(!f->closed);
  }
}

static void
init_fixture (fixture_t *f, int expected_close_status) {
  memset(f, 0, sizeof(*f));
  f->expected_close_status = expected_close_status;

  int err = uv_loop_init(&f->loop);
  assert(err == 0);
  err = udx_init(&f->loop, &f->udx, NULL);
  assert(err == 0);
  err = udx_socket_init(&f->udx, &f->sender, on_socket_close);
  assert(err == 0);
  err = udx_socket_init(&f->udx, &f->receiver, on_socket_close);
  assert(err == 0);
  f->receiver.data = f;

  err = uv_ip4_addr("127.0.0.1", 0, &f->sender_addr);
  assert(err == 0);
  err = udx_socket_bind(&f->sender, (struct sockaddr *) &f->sender_addr, 0);
  assert(err == 0);
  int addr_len = sizeof(f->sender_addr);
  err = udx_socket_getsockname(&f->sender, (struct sockaddr *) &f->sender_addr, &addr_len);
  assert(err == 0);
  err = uv_ip4_addr("127.0.0.1", 0, &f->receiver_addr);
  assert(err == 0);
  err = udx_socket_bind(&f->receiver, (struct sockaddr *) &f->receiver_addr, 0);
  assert(err == 0);
  addr_len = sizeof(f->receiver_addr);
  err = udx_socket_getsockname(&f->receiver, (struct sockaddr *) &f->receiver_addr, &addr_len);
  assert(err == 0);
  err = udx_socket_recv_start(&f->receiver, on_receiver_read);
  assert(err == 0);

  err = udx_stream_init(&f->udx, &f->stream, 1, on_stream_close, on_stream_finalize);
  assert(err == 0);
  f->stream.data = f;
  assert(f->stream.delivery_timeout_ms == 60000);
  assert(!uv_is_active((uv_handle_t *) &f->stream.delivery_timer));
  err = udx_stream_connect(&f->stream, &f->sender, 2, (struct sockaddr *) &f->receiver_addr);
  assert(err == 0);

  err = uv_timer_init(&f->loop, &f->step_timer);
  assert(err == 0);
  f->step_timer.data = f;
  err = uv_timer_init(&f->loop, &f->watchdog);
  assert(err == 0);
  err = uv_timer_start(&f->watchdog, on_watchdog, 10000, 0);
  assert(err == 0);
}

static void
set_timeout (fixture_t *f, uint32_t timeout) {
  int err = udx_stream_set_delivery_timeout(&f->stream, timeout);
  assert(err == 0);
  assert(f->stream.delivery_timeout_ms == timeout);
}

static void
write_packet (fixture_t *f, bool end) {
  unsigned int index = f->writes_started++;
  assert(index < 4);
  f->writes[index] = malloc(udx_stream_write_sizeof(1));
  assert(f->writes[index] != NULL);
  uv_buf_t buf = uv_buf_init(&f->payloads[index], 1);
  int err;
  if (end) {
    err = udx_stream_write_end(f->writes[index], &f->stream, NULL, 0, on_write_acked);
  } else {
    err = udx_stream_write(f->writes[index], &f->stream, &buf, 1, on_write_acked);
  }
  assert(err >= 0);
}

static void
wait_for_packet (fixture_t *f, unsigned int seq) {
  while (f->transmits[seq] == 0) {
    uv_run(&f->loop, UV_RUN_ONCE);
    assert(!f->closed);
  }
}

static void
on_ack_sent (udx_socket_send_t *req, int status) {
  fixture_t *f = req->data;
  assert(status == 0);
  f->ack_sent = true;
}

static void
send_ack (fixture_t *f, uint32_t ack, uint32_t sack_start, uint32_t sack_end) {
  ack_packet_t *packet = &f->ack_packet;
  memset(packet, 0, sizeof(*packet));
  packet->magic = 0xff;
  packet->version = 1;
  packet->id = udx__swap_uint32_if_be(1);
  packet->rwnd = udx__swap_uint32_if_be(UINT32_MAX);
  packet->ack = udx__swap_uint32_if_be(ack);
  size_t len = UDX_HEADER_SIZE;
  if (sack_start != sack_end) {
    packet->type = UDX_HEADER_SACK;
    packet->data_offset = 8;
    packet->sack_start = udx__swap_uint32_if_be(sack_start);
    packet->sack_end = udx__swap_uint32_if_be(sack_end);
    len += 8;
  }

  uint64_t packets_rx = f->stream.packets_rx;
  f->ack_sent = false;
  f->ack_req.data = f;
  uv_buf_t buf = uv_buf_init((char *) packet, len);
  int err = udx_socket_send(&f->ack_req, &f->receiver, &buf, 1, (struct sockaddr *) &f->sender_addr, on_ack_sent);
  assert(err == 0);
  while (!f->ack_sent || f->stream.packets_rx == packets_rx) {
    uv_run(&f->loop, UV_RUN_ONCE);
    assert(!f->closed);
  }
}

static uint64_t
delivery_deadline (fixture_t *f) {
  assert(uv_is_active((uv_handle_t *) &f->stream.delivery_timer));
  return uv_now(&f->loop) + uv_timer_get_due_in(&f->stream.delivery_timer);
}

static void
finish_fixture (fixture_t *f) {
  int err = uv_run(&f->loop, UV_RUN_DEFAULT);
  assert(err == 0);
  assert(f->closed);
  assert(f->finalized);
  assert(f->writes_acked + f->writes_cancelled == f->writes_started);
  err = uv_loop_close(&f->loop);
  assert(err == 0);
  for (unsigned int i = 0; i < f->writes_started; i++) free(f->writes[i]);
}

static void
test_timeout_and_end (bool end) {
  fixture_t f;
  init_fixture(&f, UV_ETIMEDOUT);
  set_timeout(&f, 40);
  write_packet(&f, end);
  finish_fixture(&f);
  assert(f.writes_cancelled == 1);
  // The independent deadline expires before the initial retransmission timer.
  assert(f.stream.rto_count == 0);
}

static void
test_delivery_progress (void) {
  fixture_t f;
  init_fixture(&f, 0);
  set_timeout(&f, 1000);
  write_packet(&f, false);
  wait_for_packet(&f, 0);
  uint64_t progress = f.stream.delivery_progress_ts;
  uint64_t deadline = delivery_deadline(&f);

  wait_ms(&f, 20);
  write_packet(&f, false);
  wait_for_packet(&f, 1);
  write_packet(&f, false);
  wait_for_packet(&f, 2);
  assert(f.stream.delivery_progress_ts == progress);
  assert(delivery_deadline(&f) == deadline);

  send_ack(&f, 0, 0, 0);
  assert(f.stream.delivery_progress_ts == progress);
  assert(delivery_deadline(&f) == deadline);
  send_ack(&f, 0, 2, 3);
  assert(f.unordered_acks == 1);
  assert(f.stream.delivery_progress_ts == progress);
  assert(delivery_deadline(&f) == deadline);

  // A cumulative ACK advances the deadline even with another packet missing.
  send_ack(&f, 1, 0, 0);
  assert(f.stream.remote_acked == 1);
  assert(f.stream.delivery_progress_ts > progress);
  assert(delivery_deadline(&f) > deadline);
  progress = f.stream.delivery_progress_ts;

  // Cumulatively ACKing packets already removed by SACK must still stop it.
  send_ack(&f, 1, 1, 2);
  assert(f.unordered_acks == 2);
  assert(f.stream.delivery_progress_ts == progress);
  send_ack(&f, 3, 0, 0);
  assert(f.stream.remote_acked == f.stream.seq);
  assert(f.writes_acked == 3);
  assert(!uv_is_active((uv_handle_t *) &f.stream.delivery_timer));
  set_timeout(&f, 20);
  wait_ms(&f, 40);
  assert(!f.closed);

  int err = udx_stream_destroy(&f.stream);
  assert(err >= 0);
  finish_fixture(&f);
}

static void
test_reenable_counts_elapsed_time (void) {
  fixture_t f;
  init_fixture(&f, UV_ETIMEDOUT);
  set_timeout(&f, 1000);
  write_packet(&f, false);
  wait_for_packet(&f, 0);
  uint64_t progress = f.stream.delivery_progress_ts;
  set_timeout(&f, 0);
  assert(!uv_is_active((uv_handle_t *) &f.stream.delivery_timer));
  wait_ms(&f, 40);

  set_timeout(&f, 20);
  assert(f.stream.delivery_progress_ts == progress);
  assert(uv_timer_get_due_in(&f.stream.delivery_timer) == 0);
  // Changing the setting must not close synchronously from inside the setter.
  assert(!f.closed);
  finish_fixture(&f);
  assert(f.writes_cancelled == 1);
}

static void
test_retransmit_preserves_progress (void) {
  fixture_t f;
  init_fixture(&f, UV_ETIMEDOUT);
  set_timeout(&f, 0);
  f.stream.rto = 20;
  write_packet(&f, false);
  wait_for_packet(&f, 0);
  uint64_t progress = f.stream.delivery_progress_ts;
  while (f.stream.rto_count == 0) {
    uv_run(&f.loop, UV_RUN_ONCE);
    assert(!f.closed);
  }
  assert(f.stream.retransmit_count > 0);
  assert(f.stream.delivery_progress_ts == progress);
  set_timeout(&f, 1);
  assert(uv_timer_get_due_in(&f.stream.delivery_timer) == 0);
  finish_fixture(&f);
}

static void
test_buffered_zero_window_is_out_of_scope (void) {
  fixture_t f;
  init_fixture(&f, 0);
  set_timeout(&f, 20);
  f.stream.send_rwnd = 0;
  write_packet(&f, false);
  assert(f.stream.seq == f.stream.remote_acked);
  assert(f.stream.writes_queued_bytes == 1);
  assert(!uv_is_active((uv_handle_t *) &f.stream.delivery_timer));
  wait_ms(&f, 40);
  assert(!f.closed);

  int err = udx_stream_destroy(&f.stream);
  assert(err >= 0);
  finish_fixture(&f);
  assert(f.writes_cancelled == 1);
}

static void
test_relay_stops_delivery_timer (void) {
  fixture_t f;
  udx_stream_t destination;
  init_fixture(&f, 0);
  set_timeout(&f, 1000);
  write_packet(&f, false);
  wait_for_packet(&f, 0);
  assert(uv_is_active((uv_handle_t *) &f.stream.delivery_timer));

  int err = udx_stream_init(&f.udx, &destination, 3, NULL, NULL);
  assert(err == 0);
  err = udx_stream_relay_to(&f.stream, &destination);
  assert(err == 0);
  assert(!uv_is_active((uv_handle_t *) &f.stream.delivery_timer));
  // Relaying streams use sequence numbers for forwarding. A setter must not
  // interpret their sequence gap as a new locally owned delivery deadline.
  assert(f.stream.seq != f.stream.remote_acked);
  set_timeout(&f, 20);
  assert(!uv_is_active((uv_handle_t *) &f.stream.delivery_timer));
  wait_ms(&f, 40);
  assert(!f.closed);

  // Closing the destination also destroys streams that relay to it.
  err = udx_stream_destroy(&destination);
  assert(err >= 0);
  finish_fixture(&f);
  assert(f.writes_cancelled == 1);
}

int
main (void) {
  test_timeout_and_end(false);
  test_timeout_and_end(true);
  test_delivery_progress();
  test_reenable_counts_elapsed_time();
  test_retransmit_preserves_progress();
  test_buffered_zero_window_is_out_of_scope();
  test_relay_stops_delivery_timer();
  return 0;
}

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/endian.h"

/*
 * Regression test for a small zero-window write whose initial data-carrying
 * ZWP is lost. The initial ZWP can consume the whole write queue, leaving only
 * in-flight data. When the ZWP timer fires, send_new_packet(ZWP) cannot queue
 * new data, but the sender must still retransmit and keep the ZWP timer alive
 * until the in-flight bytes are acknowledged.
 */

#define DATA_SIZE                    16
#define DROPPED_DATA_PACKETS         1
#define DROPPED_ZERO_WINDOW_ACKS     1
#define EXPECTED_ZERO_WINDOW_PROBES  (DROPPED_DATA_PACKETS + DROPPED_ZERO_WINDOW_ACKS)
#define TEST_TIMEOUT_MS              5000

uv_loop_t loop;
udx_t udx;

udx_socket_t recv_sock;
udx_stream_t recv_stream;

udx_socket_t send_sock;
udx_stream_t send_stream;

uv_udp_t proxy_to_recv;
uv_udp_t proxy_to_send;

struct sockaddr_in recv_addr;
struct sockaddr_in send_addr;
struct sockaddr_in proxy_to_recv_addr;
struct sockaddr_in proxy_to_send_addr;

uv_timer_t timeout;
udx_stream_write_t *req;

bool ack_called;
int read_counter;
int dropped_data_packets;
int dropped_zero_window_acks;
int packets_to_recv;

typedef struct {
  uv_udp_send_t req;
  char data[];
} proxy_send_t;

static uint32_t
read_u32 (const char *buf) {
  uint32_t value;
  memcpy(&value, buf, sizeof(value));
  return udx__swap_uint32_if_be(value);
}

uint32_t
pretend_buffer_is_full (udx_stream_t *stream) {
  return stream->recv_rwnd_max;
}

void
on_proxy_alloc (uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
  buf->base = malloc(suggested_size);
  buf->len = suggested_size;
}

void
on_proxy_send (uv_udp_send_t *req, int status) {
  assert(status == 0);
  free(req);
}

void
proxy_forward (uv_udp_t *proxy, const char *data, ssize_t len, const struct sockaddr *to) {
  proxy_send_t *send = malloc(sizeof(*send) + len);
  memcpy(send->data, data, len);

  uv_buf_t buf = uv_buf_init(send->data, len);
  int e = uv_udp_send(&send->req, proxy, &buf, 1, to, on_proxy_send);
  assert(e == 0);
}

void
on_proxy_to_recv (uv_udp_t *proxy, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *from, unsigned flags) {
  if (nread > 0) {
    if (dropped_data_packets < DROPPED_DATA_PACKETS) {
      dropped_data_packets++;
      free(buf->base);
      return;
    }

    packets_to_recv++;
    proxy_forward(proxy, buf->base, nread, (const struct sockaddr *) &recv_addr);
  }

  free(buf->base);
}

void
on_proxy_to_send (uv_udp_t *proxy, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *from, unsigned flags) {
  if (nread > 0) {
    uint32_t rwnd = nread >= UDX_HEADER_SIZE ? read_u32(buf->base + 8) : UINT32_MAX;
    uint32_t ack = nread >= UDX_HEADER_SIZE ? read_u32(buf->base + 16) : 0;

    // Drop the first ACK after the retransmitted data arrives. For this small
    // write, the sender has moved all bytes out of write_queue and only has
    // in-flight data left.
    if (rwnd == 0 && ack > 0 && dropped_zero_window_acks < DROPPED_ZERO_WINDOW_ACKS) {
      dropped_zero_window_acks++;
      free(buf->base);
      return;
    }

    proxy_forward(proxy, buf->base, nread, (const struct sockaddr *) &send_addr);
  }

  free(buf->base);
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  read_counter++;
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  assert(unordered == 0);
  ack_called = true;
  uv_stop(&loop);
}

void
on_timeout (uv_timer_t *timer) {
  uv_stop(timer->loop);
}

void
bind_addr (struct sockaddr_in *addr, int port) {
  int e = uv_ip4_addr("127.0.0.1", port, addr);
  assert(e == 0);
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  e = uv_loop_init(&loop);
  assert(e == 0);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  bind_addr(&recv_addr, 9101);
  bind_addr(&send_addr, 9102);
  bind_addr(&proxy_to_recv_addr, 9103);
  bind_addr(&proxy_to_send_addr, 9104);

  e = uv_udp_init(&loop, &proxy_to_recv);
  assert(e == 0);
  e = uv_udp_bind(&proxy_to_recv, (const struct sockaddr *) &proxy_to_recv_addr, 0);
  assert(e == 0);
  e = uv_udp_recv_start(&proxy_to_recv, on_proxy_alloc, on_proxy_to_recv);
  assert(e == 0);

  e = uv_udp_init(&loop, &proxy_to_send);
  assert(e == 0);
  e = uv_udp_bind(&proxy_to_send, (const struct sockaddr *) &proxy_to_send_addr, 0);
  assert(e == 0);
  e = uv_udp_recv_start(&proxy_to_send, on_proxy_alloc, on_proxy_to_send);
  assert(e == 0);

  e = udx_socket_init(&udx, &recv_sock, NULL);
  assert(e == 0);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, NULL);
  assert(e == 0);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &recv_stream, 1, NULL, NULL);
  assert(e == 0);
  e = udx_stream_init(&udx, &send_stream, 2, NULL, NULL);
  assert(e == 0);

  recv_stream.get_read_buffer_size = &pretend_buffer_is_full;
  send_stream.send_rwnd = 0;

  e = udx_stream_connect(&recv_stream, &recv_sock, 2, (struct sockaddr *) &proxy_to_send_addr);
  assert(e == 0);
  e = udx_stream_connect(&send_stream, &send_sock, 1, (struct sockaddr *) &proxy_to_recv_addr);
  assert(e == 0);

  e = udx_stream_read_start(&recv_stream, on_read);
  assert(e == 0);

  char data[DATA_SIZE];
  memset(data, 0, sizeof(data));
  uv_buf_t buf = uv_buf_init(data, sizeof(data));

  e = udx_stream_write(req, &send_stream, &buf, 1, on_ack);
  assert(e && "drained");

  e = uv_timer_init(&loop, &timeout);
  assert(e == 0);
  e = uv_timer_start(&timeout, on_timeout, TEST_TIMEOUT_MS, 0);
  assert(e == 0);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(dropped_data_packets == DROPPED_DATA_PACKETS);
  assert(dropped_zero_window_acks == DROPPED_ZERO_WINDOW_ACKS);
  assert(read_counter == 1);
  // The timeout must retransmit follow-up probes even though
  // send_new_packet(ZWP) cannot queue more data. Dropping the initial data and
  // the first ACK makes this require both retransmission and re-arming ZWP.
  assert(send_stream.zwp_count >= EXPECTED_ZERO_WINDOW_PROBES);
  assert(packets_to_recv >= EXPECTED_ZERO_WINDOW_PROBES);
  assert(ack_called);
  assert(send_stream.writes_queued_bytes == 0);

  free(req);

  return 0;
}

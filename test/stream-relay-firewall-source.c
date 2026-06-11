#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

#define NBYTES_TO_SEND 1000000

typedef struct {
  uv_loop_t loop;
  udx_t udx;

  struct sockaddr_in aaddr;
  udx_socket_t asock;
  udx_stream_t astream;

  struct sockaddr_in baddr;
  udx_socket_t bsock;
  udx_stream_t bstream;

  struct sockaddr_in caddr;
  udx_socket_t csock;
  udx_stream_t cstream;

  struct sockaddr_in daddr;
  udx_socket_t dsock;
  udx_stream_t dstream;

  uv_timer_t timeout;

  bool ack_called;
  bool read_called;
  bool firewall_called;

  size_t nbytes_read;

  int nclosed;
} test_case_t;

static int
on_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  test_case_t *test = stream->data;

  assert(stream == &test->astream);
  assert(socket == &test->asock);

  test->firewall_called = true;

  // Relayed packets must arrive from the paired relay stream's socket, not
  // from the incoming stream socket that happened to forward the packet.
  assert(from->sa_family == AF_INET);
  assert(((const struct sockaddr_in *) from)->sin_port == test->baddr.sin_port);

  int e = udx_stream_connect(&test->astream, socket, 2, from);
  assert(e == 0);

  return 0;
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  test_case_t *test = req->data;

  assert(status == 0);

  udx_stream_destroy(&test->astream);
  udx_stream_destroy(&test->bstream);
  udx_stream_destroy(&test->cstream);
  udx_stream_destroy(&test->dstream);

  test->ack_called = true;
}

static void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  test_case_t *test = stream->data;

  if (read_len < 0) return;

  assert(buf->len == read_len);

  if (test->nbytes_read == 0) {
    assert(memcmp(buf->base, "hello", 5) == 0);
  }

  test->nbytes_read += read_len;
  test->read_called = true;
}

static void
on_socket_close (udx_socket_t *socket) {
  (void) socket;
}

static void
on_close (udx_stream_t *stream, int err) {
  test_case_t *test = stream->data;

  test->nclosed++;

  if (test->nclosed == 4) {
    uv_timer_stop(&test->timeout);
    uv_close((uv_handle_t *) &test->timeout, NULL);

    udx_socket_close(&test->asock);
    udx_socket_close(&test->bsock);
    udx_socket_close(&test->csock);
    udx_socket_close(&test->dsock);
  }
}

static void
on_timeout (uv_timer_t *timer) {
  assert(false && "relay source firewall test timed out");
}

static void
bind_addr (udx_socket_t *socket, struct sockaddr_in *addr, int port) {
  int e = uv_ip4_addr("127.0.0.1", port, addr);
  assert(e == 0);

  e = udx_socket_bind(socket, (struct sockaddr *) addr, 0);
  assert(e == 0);
}

static void
run_case (int base_port, bool force_slow_path) {
  test_case_t test = { 0 };

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  req->data = &test;

  int e = uv_loop_init(&test.loop);
  assert(e == 0);

  e = udx_init(&test.loop, &test.udx, NULL);
  assert(e == 0);

  if (force_slow_path) {
    test.udx.debug_flags |= UDX_DEBUG_FORCE_RELAY_SLOW_PATH;
  }

  e = udx_socket_init(&test.udx, &test.asock, on_socket_close);
  assert(e == 0);

  e = udx_socket_init(&test.udx, &test.bsock, on_socket_close);
  assert(e == 0);

  e = udx_socket_init(&test.udx, &test.csock, on_socket_close);
  assert(e == 0);

  e = udx_socket_init(&test.udx, &test.dsock, on_socket_close);
  assert(e == 0);

  // Use a distinct socket/port per stream so a wrong forwarding socket is
  // observable as the wrong UDP source port in the firewall callback.
  bind_addr(&test.asock, &test.aaddr, base_port + 1);
  bind_addr(&test.bsock, &test.baddr, base_port + 2);
  bind_addr(&test.csock, &test.caddr, base_port + 3);
  bind_addr(&test.dsock, &test.daddr, base_port + 4);

  e = udx_stream_init(&test.udx, &test.astream, 1, on_close, NULL);
  assert(e == 0);
  test.astream.data = &test;

  e = udx_stream_init(&test.udx, &test.bstream, 2, on_close, NULL);
  assert(e == 0);
  test.bstream.data = &test;

  e = udx_stream_init(&test.udx, &test.cstream, 3, on_close, NULL);
  assert(e == 0);
  test.cstream.data = &test;

  e = udx_stream_init(&test.udx, &test.dstream, 4, on_close, NULL);
  assert(e == 0);
  test.dstream.data = &test;

  // A is the firewalled destination; on_firewall asserts that forwarded relay
  // traffic arrives from B's socket, not from C's incoming stream socket.
  e = udx_stream_firewall(&test.astream, on_firewall);
  assert(e == 0);

  // Pair B and C as the relay: data received by C should be forwarded to A
  // using B's socket, which is the source A is expecting.
  e = udx_stream_relay_to(&test.cstream, &test.bstream);
  assert(e == 0);

  e = udx_stream_relay_to(&test.bstream, &test.cstream);
  assert(e == 0);

  e = udx_stream_read_start(&test.astream, on_read);
  assert(e == 0);

  // B is the relay leg visible to A; C/D create the incoming relay leg that
  // triggers forwarding when D writes below.
  e = udx_stream_connect(&test.bstream, &test.bsock, 1, (struct sockaddr *) &test.aaddr);
  assert(e == 0);

  e = udx_stream_connect(&test.cstream, &test.csock, 4, (struct sockaddr *) &test.daddr);
  assert(e == 0);

  e = udx_stream_connect(&test.dstream, &test.dsock, 3, (struct sockaddr *) &test.caddr);
  assert(e == 0);

  char *data = calloc(NBYTES_TO_SEND, 1);
  uv_buf_t buf = uv_buf_init(data, NBYTES_TO_SEND);

  memcpy(buf.base, "hello", 5);

  uv_timer_init(&test.loop, &test.timeout);
  uv_timer_start(&test.timeout, on_timeout, 5000, 0);

  // Writing from D into C exercises relay_packet(cstream -> bstream). The
  // regression sent this packet from C's socket instead of B's socket.
  udx_stream_write(req, &test.dstream, &buf, 1, on_ack);

  e = uv_run(&test.loop, UV_RUN_DEFAULT);
  assert(e == 0);

  e = uv_loop_close(&test.loop);
  assert(e == 0);

  // Ensure the firewalled path was actually used and data made it through.
  assert(test.firewall_called);
  assert(test.ack_called && test.read_called);
  assert(test.nbytes_read == NBYTES_TO_SEND);

  free(req);
  free(data);
}

int
main () {
  run_case(8080, false);
  run_case(8090, true);

  return 0;
}

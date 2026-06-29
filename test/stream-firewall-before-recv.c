#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

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

uv_timer_t timeout;

udx_stream_send_t first_req;
udx_stream_send_t second_req;

bool firewall_called = false;
int recv_count = 0;

static void
on_timeout (uv_timer_t *timer) {
  assert(false && "stream firewall before recv test timed out");
}

static void
bind_addr (udx_socket_t *socket, struct sockaddr_in *addr) {
  int e;

  uv_ip4_addr("127.0.0.1", 0, addr);
  e = udx_socket_bind(socket, (struct sockaddr *) addr, 0);
  assert(e == 0);

  int len = sizeof(*addr);
  e = uv_udp_getsockname(&socket->uv_udp, (struct sockaddr *) addr, &len);
  assert(e == 0);
}

static void
on_send (udx_stream_send_t *req, int status) {
  assert(status == 0);
}

static int
on_firewall (udx_stream_t *stream, udx_socket_t *socket, const struct sockaddr *from) {
  assert(stream == &astream);
  assert(socket == &asock);
  assert(recv_count == 1);
  assert(!firewall_called);

  firewall_called = true;

  return 0;
}

static void
on_recv (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  int e;

  assert(stream == &astream);

  if (recv_count == 0) {
    assert(read_len == 5);
    assert(memcmp(buf->base, "first", 5) == 0);
    assert(!firewall_called);

    recv_count++;

    // Move the stream to another socket. The peer still sends to the previous
    // address, so the next raw message must pass through the firewall callback.
    e = udx_stream_change_remote(&astream, &csock, 2, (struct sockaddr *) &baddr, NULL);
    assert(e == 1);

    uv_buf_t second = uv_buf_init("second", 6);
    e = udx_stream_send(&second_req, &bstream, &second, 1, on_send);
    assert(e == 0);

    return;
  }

  assert(read_len == 6);
  assert(memcmp(buf->base, "second", 6) == 0);
  assert(firewall_called);

  recv_count++;

  uv_timer_stop(&timeout);
  uv_stop(&loop);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &asock, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &bsock, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &csock, NULL);
  assert(e == 0);

  bind_addr(&asock, &aaddr);
  bind_addr(&bsock, &baddr);
  bind_addr(&csock, &caddr);

  e = udx_stream_init(&udx, &astream, 1, NULL, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, NULL, NULL);
  assert(e == 0);

  e = udx_stream_firewall(&astream, on_firewall);
  assert(e == 0);

  e = udx_stream_recv_start(&astream, on_recv);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  uv_timer_init(&loop, &timeout);
  uv_timer_start(&timeout, on_timeout, 3000, 0);

  uv_buf_t first = uv_buf_init("first", 5);
  e = udx_stream_send(&first_req, &bstream, &first, 1, on_send);
  assert(e == 0);

  uv_run(&loop, UV_RUN_DEFAULT);

  assert(firewall_called);
  assert(recv_count == 2);

  return 0;
}

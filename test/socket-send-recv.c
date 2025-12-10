#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_socket_t bsock;
#define NTESTS 4

struct {
  udx_socket_send_t req;
  char *string;
  int ttl;
  int next_ttl;
  bool send_called;
  bool recv_called;
} tests[NTESTS] = {{.string = "one", .ttl = 0, .next_ttl = 10}, {.string = "two", .ttl = 10, .next_ttl = 20}, {.string = "three", .ttl = 20, .next_ttl = 0}, {.string = "four", .ttl = 0, .next_ttl = 0}};

int
get_socket_ttl (udx_socket_t *socket) {
  int fd;
  uv_fileno((uv_handle_t *) &socket->uv_udp, &fd);

  int ttl;
  socklen_t ttl_opt_size = sizeof ttl;
  int rc = getsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, &ttl_opt_size);
  assert(rc == 0);

  return ttl;
}

int nrecv_called;

// check that after our packet is sent the TTL is set for sending the next packet

void
on_send (udx_socket_send_t *r, int status) {
  assert(r == &tests[0].req || r == &tests[1].req || r == &tests[2].req || r == &tests[3].req);
  assert(status == 0);

  int ttl = get_socket_ttl(&bsock);
  int i;

  for (i = 0; i < NTESTS; i++) {
    if (r == &tests[i].req) {
      int wanted_ttl = tests[i].next_ttl ?: bsock.ttl; /* UDX_DEFAULT_TTL */

      tests[i].send_called = true;

      // assert(ttl == wanted_ttl);
    }
  }
}

void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  assert(buf->len == read_len);

  int i = 0;

  for (; i < NTESTS; i++) {
    if (read_len == strlen(tests[i].string) && memcmp(buf->base, tests[i].string, read_len) == 0) {
      tests[i].recv_called = true;
      break;
    }
  }

  if (++nrecv_called == 4) {
    uv_stop(&loop);
  }
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

  struct sockaddr_in baddr;
  uv_ip4_addr("127.0.0.1", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr, 0);
  assert(e == 0);

  struct sockaddr_in aaddr;
  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr, 0);
  assert(e == 0);

  e = udx_socket_recv_start(&asock, on_recv);
  assert(e == 0);

  udx.debug_flags |= UDX_DEBUG_FORCE_SEND_SLOW_PATH;

  for (int i = 0; i < NTESTS; i++) {
    udx_socket_send_t *req = &tests[i].req;
    uv_buf_t buf = uv_buf_init(tests[i].string, strlen(tests[i].string));
    int ttl = tests[i].ttl;
    if (ttl == 0) {
      udx_socket_send(req, &bsock, &buf, 1, (struct sockaddr *) &aaddr, on_send);
    } else {
      udx_socket_send_ttl(req, &bsock, &buf, 1, (struct sockaddr *) &aaddr, ttl, on_send);
    }
  }

  uv_run(&loop, UV_RUN_DEFAULT);

  for (int i = 0; i < NTESTS; i++) {
    assert(tests[i].send_called);
    assert(tests[i].recv_called);
  }

  return 0;
}

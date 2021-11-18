#include <uv.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include "../src/ucp.h"
#include "../src/fifo.h"
#include "../src/cirbuf.h"

static ucp_t server;
static ucp_stream_t server_sock;
static ucp_send_t sreq;
static uv_timer_t timer;
static uint32_t sbuf;

static size_t rcvd = 0;
static size_t reads = 0;

static void
on_uv_interval (uv_timer_t *req) {
  ucp_stream_send_state(&server_sock);
}

static void
on_read (ucp_stream_t *stream, char *buf, size_t read) {
  rcvd += read;
  printf("on read, data_size=%zu, total recv=%zu total reads=%zu\n", read, rcvd, ++reads);
}

static void
on_message (ucp_t *self, char *buf, ssize_t nread, const struct sockaddr_in *from) {
  if (nread < 4) return;

  ucp_set_callback(&server, UCP_ON_MESSAGE, NULL);

  uint32_t id = *((uint32_t *) buf);
  printf("remote socket id: %u\n", id);

  ucp_stream_connect(&server_sock, id, (const struct sockaddr *) from);

  sbuf = server_sock.local_id;
  ucp_send(&server, &sreq, (char *) &sbuf, 4, (const struct sockaddr *) from);
  uv_timer_start(&timer, on_uv_interval, 20, 20);
}

int
main () {
  srand(time(0));

  uv_loop_t* loop = malloc(sizeof(uv_loop_t));
  uv_loop_init(loop);

  struct sockaddr_in addr;

  ucp_init(&server, loop);
  uv_timer_init(loop, &timer);

  int b = 2 * 1024 * 1024;
  ucp_send_buffer_size(&server, &b);
  ucp_recv_buffer_size(&server, &b);

  uv_ip4_addr("0.0.0.0", 10101, &addr);
  ucp_bind(&server, (const struct sockaddr *) &addr);
  ucp_set_callback(&server, UCP_ON_MESSAGE, on_message);

  ucp_stream_init(&server, &server_sock);
  ucp_stream_set_callback(&server_sock, UCP_ON_READ, on_read);

  printf("local socket id: %u\n", server_sock.local_id);

  uv_run(loop, UV_RUN_DEFAULT);
  free(loop);

  return 0;
}

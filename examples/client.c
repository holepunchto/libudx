#include <stdio.h>
#include <stdlib.h>
#include <uv.h>

#include "../include/udx.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

static uv_loop_t loop;
static udx_t udx;

static udx_socket_t sock;
static udx_socket_send_t req;

static udx_stream_t stream;
static struct sockaddr_in dest_addr;

static size_t bytes_recv = 0;
static uint64_t started = 0;

static uint32_t client_id = 1;
static uint32_t server_id = 2;

static uv_timer_t timer;

static uint64_t
get_milliseconds () {
  return uv_hrtime() / 1000000;
}

static void
on_uv_interval (uv_timer_t *handle) {
  printf("received %zu bytes in %lu ms\n", bytes_recv, get_milliseconds() - started);
}

static void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  if (started == 0) {
    started = get_milliseconds();
    uv_timer_init(&loop, &timer);
    uv_timer_start(&timer, on_uv_interval, 5000, 5000);
  }

  if (read_len < 0) {
    printf("received %zu bytes in %lu ms\n", bytes_recv, get_milliseconds() - started);
    printf("stream is done!\n");
    exit(0);
  }

  bytes_recv += read_len;
}

static void
on_send (udx_socket_send_t *r, int status) {
  udx_stream_init(&udx, &stream, client_id, NULL, NULL);
  udx_stream_connect(&stream, &sock, server_id, (struct sockaddr *) &dest_addr);
  udx_stream_read_start(&stream, on_read);
}

int
main (int argc, char **argv) {
  if (argc < 2) return 1;

  uv_ip4_addr(argv[1], 18081, &dest_addr);

  uv_loop_init(&loop);

  udx_init(&loop, &udx);

  udx_socket_init(&udx, &sock);

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", 18082, &addr);

  udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);

  client_id = (uint32_t) getpid();
  server_id = client_id + 1;

  uint32_t ids[2] = {client_id, server_id};

  uv_buf_t buf = uv_buf_init((char *) ids, 8);
  udx_socket_send(&req, &sock, &buf, 1, (struct sockaddr *) &dest_addr, on_send);

  uv_run(&loop, UV_RUN_DEFAULT);
  return 0;
}

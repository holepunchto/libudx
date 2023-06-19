#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_stream_t astream;

udx_socket_t bsock;
udx_stream_t bstream;

udx_stream_write_t req;

struct {
  uint64_t size_bytes;
} options;

struct {
  uint64_t bytes_read;
  uint64_t last_bytes_read;

  uint64_t last_print_ms;
  uint64_t time_zero_ms;

  uint64_t last_read_ms;
  int finished;
} stats;

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  printf("write acked, status=%d %s\n", status, status == UV_ECANCELED ? "(UV_ECANCELED)" : "");
  udx_stream_destroy(r->handle);
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  stats.bytes_read += read_len;
  stats.last_read_ms = uv_hrtime() / 1000000;

  if (stats.bytes_read == options.size_bytes) {
    // udx_stream_destroy(handle);
    printf("read all bytes\n");
    // __builtin_trap();
  }
}

static void
on_b_sock_close () {
  printf("sending socket closing\n");
}

static void
on_b_stream_close () {
  printf("sending stream closing\n");
  int e = udx_socket_close(&bsock, on_b_sock_close);
  assert(e == 0 && "udx_socket_close (sender, 'b')");
}

static void
on_a_sock_close () {
  printf("receiving socket closing\n");
}

static void
on_a_stream_close () {
  printf("receiving stream closing\n");
  int e = udx_socket_close(&asock, on_a_sock_close);
  assert(e == 0 && "udx_socket_close (receiver, 'a')");
}
int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx);
  assert(e == 0);

  e = udx_socket_init(&udx, &asock);
  assert(e == 0);

  e = udx_socket_init(&udx, &bsock);
  assert(e == 0);

  struct sockaddr_in baddr;
  uv_ip4_addr("127.0.0.1", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr);
  assert(e == 0);

  struct sockaddr_in aaddr;
  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, on_a_stream_close);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, on_b_stream_close);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  printf("generating data ...\n");

  // options.size_bytes = 2 * 1024 * 1024 * 1024L;
  options.size_bytes = 2 * 1024 * 1024L;

  uint8_t *data = calloc(options.size_bytes, 1);

  assert(data != NULL && "malloc");

  printf("writing data\n");

  uv_buf_t buf = uv_buf_init(data, options.size_bytes);
  udx_stream_write(&req, &bstream, &buf, 1, on_ack);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0 && "UV_RUN");

  uv_loop_close(&loop);

  // just for valgrind
  free(data);
  return 0;
}

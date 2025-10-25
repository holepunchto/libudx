#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "helpers.h"

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_stream_t astream;

udx_socket_t bsock;
udx_stream_t bstream;

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

uint64_t read_hash = HASH_INIT;
uint64_t write_hash = HASH_INIT;

uv_timer_t stat_timer;

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  printf("write acked, status=%d %s\n", status, status == UV_ECANCELED ? "(UV_ECANCELED)" : "");
  uv_timer_stop(&stat_timer);
  udx_stream_destroy(r->stream);
  udx_stream_destroy(&astream);
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  stats.bytes_read += read_len;
  stats.last_read_ms = uv_hrtime() / 1000000;

  assert(read_len == buf->len);

  read_hash = hash(read_hash, (uint8_t *) buf->base, read_len);

  if (stats.bytes_read == options.size_bytes) {
    printf("read all bytes\n");
  }
}

static void
on_b_sock_close () {
  printf("sending socket closing\n");
}

static void
on_b_stream_close () {
  printf("sending stream closing\n");
  int e = udx_socket_close(&bsock);
  assert(e == 0 && "udx_socket_close (sender, 'b')");
}

static void
on_a_sock_close () {
  printf("receiving socket closing\n");
}

static void
on_a_stream_close () {
  printf("receiving stream closing\n");
  int e = udx_socket_close(&asock);
  assert(e == 0 && "udx_socket_close (receiver, 'a')");
}

static void
print_rate_on_interval (uv_timer_t *t) {
  uint64_t a_bw;
  uint64_t b_bw;

  udx_stream_get_bw(&astream, &a_bw);
  udx_stream_get_bw(&bstream, &b_bw);

  double a_rate = 0;
  double b_rate = 0;

  if (astream.rate_interval_ms) {
    a_rate = astream.rate_delivered / (1.0f * astream.rate_interval_ms);
  }
  if (bstream.rate_interval_ms) {
    b_rate = bstream.rate_delivered / (1.0f * bstream.rate_interval_ms);
  }

  printf("A bbr.bw=%" PRIu64 " rate=%9.3f/kpkts/sec %s\n", a_bw, a_rate, astream.rate_sample_is_app_limited ? "(app limited)" : "");
  printf("B bbr.bw=%" PRIu64 " rate=%9.3f/kpkts/sec %s\n", b_bw, b_rate, bstream.rate_sample_is_app_limited ? "(app limited)" : "");
}

int
main () {
  int e;

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &asock, on_a_sock_close);
  assert(e == 0);

  e = udx_socket_init(&udx, &bsock, on_b_sock_close);
  assert(e == 0);

  struct sockaddr_in baddr;
  uv_ip4_addr("127.0.0.1", 8082, &baddr);
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr, 0);
  assert(e == 0);

  struct sockaddr_in aaddr;
  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, on_a_stream_close, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, on_b_stream_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  options.size_bytes = 500 * 1024 * 1024L;
  printf("generating data ... (%" PRIu64 " bytes)\n", options.size_bytes);

  uint8_t *data = calloc(options.size_bytes, 1);

  write_hash = hash(write_hash, data, options.size_bytes);

  assert(data != NULL && "malloc");

  printf("writing data\n");

  uv_buf_t buf = uv_buf_init((char *) data, options.size_bytes);
  udx_stream_write(req, &bstream, &buf, 1, on_ack);

  uv_timer_init(&loop, &stat_timer);

  uv_timer_start(&stat_timer, print_rate_on_interval, 0, 200);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0 && "UV_RUN");

  uv_loop_close(&loop);

  free(data); // valgrind
  free(req);  // valgrind
  printf("readhash=%" PRIx64 " writehash=%" PRIx64 "\n", read_hash, write_hash);
  assert(read_hash == write_hash);

  printf("stats: udx:      bytes_rx=%" PRIu64 " packets_rx=%" PRIu64 " bytes_tx=%" PRIu64 " packets_tx=%" PRIu64 "\n", udx.bytes_rx, udx.packets_rx, udx.bytes_tx, udx.packets_tx);
  printf("stats: stream a: bytes_rx=%" PRIu64 " packets_rx=%" PRIu64 " bytes_tx=%" PRIu64 " packets_tx=%" PRIu64 "\n", astream.bytes_rx, astream.packets_rx, astream.bytes_tx, astream.packets_tx);
  printf("stats: stream b: bytes_rx=%" PRIu64 " packets_rx=%" PRIu64 " bytes_tx=%" PRIu64 " packets_tx=%" PRIu64 "\n", bstream.bytes_rx, bstream.packets_rx, bstream.bytes_tx, bstream.packets_tx);

  if (asock.packets_dropped_by_kernel != -1 && bsock.packets_dropped_by_kernel != -1) {
    printf("stats: socket a: packets_dropped=%" PRIi64 "\n", asock.packets_dropped_by_kernel);
    printf("stats: socket b: packets_dropped=%" PRIi64 "\n", bsock.packets_dropped_by_kernel);

    assert(asock.packets_dropped_by_kernel + bsock.packets_dropped_by_kernel == udx.packets_dropped_by_kernel);
  }

  return 0;
}

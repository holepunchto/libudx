#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <time.h>
#include <uv.h>
#include <assert.h>

#include "../include/udx.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

#define m2ns(ms) ((ms) * 1000000)
#define n2ms(ns) ((ns) / 1000000)
#define get_milliseconds(t) (uv_hrtime() / (t) 1000000)

static uv_loop_t loop;
static uv_timer_t block_timer;
static udx_t udx;

static udx_socket_t sock;
static udx_socket_send_t req;

static udx_stream_t stream;
static struct sockaddr_in dest_addr;

static uint32_t client_id = 1;
static uint32_t server_id = 2;

static struct stats_s {
  uint64_t ns_start;
  uint64_t rx_packets;
  uint64_t rx_bytes;
} s;

static uint64_t hold_ns = m2ns(25); // pauses outside
static uint64_t block_ns = 0; // pauses inside


static void
uv_sleep_nano(uint64_t nsec);


static void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  if (s.ns_start == 0) {
    s.ns_start = uv_hrtime();
  }

  s.rx_bytes +=read_len;
  s.rx_packets++;

  if (1 || !(s.rx_packets % 1000) || block_ns > 100 || hold_ns > 100) {
    printf("packets: %zu, bytes: %zu, drops: %zu\n", s.rx_packets, s.rx_bytes, sock.packets_dropped_by_kernel);
  }

  if (read_len < 0) {
    printf("received %zu bytes in %" PRIu64 " ms\n", s.rx_bytes, n2ms(uv_hrtime() - s.ns_start));
    printf("stream is done!\n");
    exit(0);
  }
}

static void
on_send (udx_socket_send_t *r, int status) {
  udx_stream_init(&udx, &stream, client_id, NULL, NULL);
  udx_stream_connect(&stream, &sock, server_id, (struct sockaddr *) &dest_addr);
  udx_stream_read_start(&stream, on_read);
}

static void
on_block (uv_timer_t *handle) {
  // printf("block! %zu\n", n2ms(block_ns));
  uv_sleep_nano(block_ns);
}

static int
uv_stutter (uv_loop_t *loop) {
  uv_timer_init(loop, &block_timer); // test uv_send async too
  int active;
  uint64_t prev = uv_hrtime();

  do {
    uv_timer_start(&block_timer, on_block, 0, 0);
    active = uv_run(loop, UV_RUN_ONCE);

    uint64_t now = uv_hrtime();
    uint64_t delta = now - prev;
    prev = now;

    if (n2ms(now - s.ns_start) < 2000) continue; // let stream warmup for 2 sec

    // block_ns = hold_ns * pow(2, i++);
    uint64_t ns = hold_ns;

    printf("%f> stop! %zu, block %zu\n", n2ms((double) delta), n2ms(ns), n2ms(block_ns));
    uv_sleep_nano(ns);
  } while (active);
  return 0;
}

int
main (int argc, char **argv) {
  if (argc < 2) return 1;
  memset(&s, 0, sizeof(s));

  uv_ip4_addr(argv[1], 18081, &dest_addr);

  if (argc > 2) {
    hold_ns = atoi(argv[2]);
    assert(hold_ns >= 0);
    hold_ns = m2ns(hold_ns);
  }

  uv_loop_init(&loop);

  udx_init(&loop, &udx, NULL);

  udx_socket_init(&udx, &sock, NULL);

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", 18082, &addr);

  udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);

  client_id = (uint32_t) getpid();
  server_id = client_id + 1;

  uint32_t ids[2] = {client_id, server_id};

  uv_buf_t buf = uv_buf_init((char *) ids, 8);
  udx_socket_send(&req, &sock, &buf, 1, (struct sockaddr *) &dest_addr, on_send);

  uv_stutter(&loop);
  return 0;
}

static void
uv_sleep_nano(uint64_t nsec) { // lifted from uv/core.c
  struct timespec timeout;
  int rc;

  timeout.tv_sec = nsec / 1000000000;
  timeout.tv_nsec = (nsec % 1000000000);

  do
    rc = nanosleep(&timeout, &timeout);
  while (rc == -1 && errno == EINTR);

  assert(rc == 0);
}

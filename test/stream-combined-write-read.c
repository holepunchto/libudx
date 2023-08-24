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

#define NUM_WRITES         20
#define SMALL_PAYLOAD_SIZE 20
// queue enough to fill the write window, otherwise the small writes
// will be sent as fast as we can queue them.
#define LARGE_PAYLOAD_SIZE 3500
// just a small write to ack and clean up
#define FINAL_PAYLOAD_SIZE 5

#define TOTAL_READ_EXPECTED (LARGE_PAYLOAD_SIZE + NUM_WRITES * SMALL_PAYLOAD_SIZE + FINAL_PAYLOAD_SIZE)
size_t total_read;
int total_acks;

udx_stream_write_t req[NUM_WRITES];
uv_buf_t buf[NUM_WRITES];

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  total_acks++;
  printf("write acked, acks=%d, status=%d %s\n", total_acks, status, status == UV_ECANCELED ? "(UV_ECANCELED)" : "");

  if (total_acks == 22) {
    udx_stream_destroy(r->handle);
    udx_stream_destroy(&astream);
  }
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len < 0) __builtin_trap();

  total_read += read_len;
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

/* Fully close a loop */
static void
close_walk_cb (uv_handle_t *handle, void *arg) {
  if (!uv_is_closing(handle))
    uv_close(handle, NULL);
}

static void
close_loop (uv_loop_t *loop) {
  uv_walk(loop, close_walk_cb, NULL);
  uv_run(loop, UV_RUN_DEFAULT);
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
  e = udx_socket_bind(&bsock, (struct sockaddr *) &baddr, 0);
  assert(e == 0);

  struct sockaddr_in aaddr;
  uv_ip4_addr("127.0.0.1", 8081, &aaddr);
  e = udx_socket_bind(&asock, (struct sockaddr *) &aaddr, 0);
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

  // first queue a write that will fill up the initial window (roughly CORK_SIZE bytes).
  // if we just queue up small writes, they'll each be written out before they can accumulate
  // and be considered for write-combining.

  printf("sizeof(udx_packet_t)=%ld\n", sizeof(udx_packet_t));

  char _payload[LARGE_PAYLOAD_SIZE];

  memset(_payload, 'a', LARGE_PAYLOAD_SIZE);
  udx_stream_write_t w;
  uv_buf_t payload = uv_buf_init(_payload, LARGE_PAYLOAD_SIZE);
  udx_stream_write(&w, &bstream, &payload, 1, on_ack);

  char small_payload[SMALL_PAYLOAD_SIZE] = "12345678901234567890";

  for (int i = 0; i < NUM_WRITES; i++) {
    printf("small_write=%d\n", i);
    buf[i] = uv_buf_init(small_payload, SMALL_PAYLOAD_SIZE);
    udx_stream_write(&req[i], &bstream, &buf[i], 1, on_ack);
  }
  char _final_payload[] = "final";
  udx_stream_write_t w2;
  uv_buf_t final_payload = uv_buf_init(_final_payload, 5);

  udx_stream_write(&w2, &bstream, &final_payload, 1, on_ack);
  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0 && "UV_RUN");

  assert(total_read == TOTAL_READ_EXPECTED);

  e = uv_loop_close(&loop);

  close_loop(&loop);

  uv_library_shutdown();

  return 0;
}

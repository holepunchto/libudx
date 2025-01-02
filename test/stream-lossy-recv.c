/**
 * Test a faulty receiver.
 *
 * The transmitter sends 1k messages over a stream each
 * earmarked with an incrementing counter.
 *
 * The receiving end was patched to drop every odd packet
 * or first 4 packets of every 8.
 */

#include "assert.h"
#include <stdlib.h>
#include <memory.h>

#include "../include/udx.h"

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_stream_t astream;

udx_socket_t bsock;
udx_stream_t bstream;

#define STRIDE 1024
#define N_SAMPLES 1024

static size_t read_offset = 0;
static uint64_t time_start = 0;
void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  if (time_start == 0) time_start = uv_hrtime();

  size_t o = 0;

  while (o < read_len) {
    if (!(read_offset % STRIDE)) {
      int i = *(uint32_t *)(buf->base + o);
      // printf("on_a_read, read_offset=%zu len=%zi i=%i\n", read_offset, read_len, i);
      double delta = (uv_hrtime() - time_start) / 1000000. / 1000.; // seconds
      printf("received message=%i e2e-packets/sec=%0.2f\n", i, i / delta);

      if (i == N_SAMPLES - 1) { // last message
        udx_stream_destroy(handle);
      }
    }
    o++;
    read_offset++;
  }
}

static void
on_a_sock_close () {
  printf("rx socket closed\n");
}

static void
on_b_sock_close () {
  printf("tx socket closed\n");
}

static void
on_a_stream_close () {
  printf("rx stream closed\n");
  udx_socket_close(&asock);
}


static void
on_b_stream_close () {
  printf("tx stream closed\n");
  udx_socket_close(&bsock);
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  free(req);
}

static void
send_data (udx_stream_t *stream) {
  printf("on drain\n");
}

int
main () {
  int e;
  e = uv_loop_init(&loop);
  assert(e == 0);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &asock, on_a_sock_close);
  assert(e == 0);
  asock.debug_force_recv_drop = 1;

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

  char *buffer = calloc(N_SAMPLES, STRIDE);
  memset(buffer, 0xaa, N_SAMPLES * STRIDE);

  for (int i = 0; i < N_SAMPLES; i++) {
    udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));

    char *data = buffer + (i * STRIDE);
    *((uint32_t *) data) = i;
    uv_buf_t buf = { .base = data, .len = STRIDE };

    e = udx_stream_write(req, &bstream, &buf, 1, on_ack); // returns UV_EEXIST?
    // assert(e == 0);

    udx_stream_write_resume(&bstream, send_data);
  }

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0 && "UV_RUN");

  uv_loop_close(&loop);

  free(buffer);
  return 0;
}

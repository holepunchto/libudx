#include <stdio.h>
#include <stdlib.h>

#include "../include/udx.h"

#define PUMP_BYTES (1024 * 1024 * 1024)

static uv_loop_t loop;
static udx_t udx;

static udx_socket_t sock;
static udx_stream_t stream;

static bool stream_is_active = false;
static bool stream_is_queued = false;
static uint32_t client_id = 0;
static uint32_t server_id = 0;
static size_t bytes_sent = 0;
static struct sockaddr_in dest_addr;

static uv_buf_t chunk;
static uv_buf_t empty = {.base = NULL, .len = 0};

static bool printed_warning = false;

static void
pump_stream ();

static void
on_close (udx_stream_t *stream, int status) {
  printf("stream closed with status %i\n", status);

  stream_is_active = false;
  bytes_sent = 0;

  if (stream_is_queued) {
    stream_is_queued = false;
    pump_stream();
  }
}

static void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  free(req);
}

static void
on_ack_end (udx_stream_write_t *req, int status, int unordered) {
  udx_stream_destroy(req->stream);
  free(req);
}

static void
pump_writes () {
  while (bytes_sent < PUMP_BYTES) {
    udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
    bytes_sent += chunk.len;

    if (udx_stream_write(req, &stream, &chunk, 1, on_ack)) continue;

    udx_stream_write_resume(&stream, pump_writes);
    return;
  }

  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  udx_stream_write_end(req, &stream, &empty, 1, on_ack_end);
}

static void
pump_stream () {
  stream_is_active = true;

  char dst_ip[20];
  uv_ip4_name(&dest_addr, dst_ip, 20);

  printf("pumping %d bytes to stream to %s...\n", PUMP_BYTES, dst_ip);

  udx_stream_init(&udx, &stream, server_id, on_close, NULL);
  udx_stream_connect(&stream, &sock, client_id, (struct sockaddr *) &dest_addr);

  pump_writes();
}

static void
on_recv (udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  if (read_len != 8) {
    if (!printed_warning) {
      printed_warning = true;
      printf("warning: unknown packet received (%zd bytes)\n", read_len);
    }
    return;
  }

  printf("client requested streams...\n");

  uint32_t *ids = (uint32_t *) buf->base;

  client_id = *(ids++);
  server_id = *(ids++);
  dest_addr = *((struct sockaddr_in *) from);

  if (stream_is_active) {
    stream_is_queued = true;
    udx_stream_destroy(&stream);
    return;
  }

  pump_stream();
}

int
main (int argc, char **argv) {
  uv_loop_init(&loop);

  chunk.len = 16384;
  chunk.base = calloc(1, chunk.len);

  udx_init(&loop, &udx, NULL);

  udx_socket_init(&udx, &sock, NULL);

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", 18081, &addr);

  udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);

  udx_socket_recv_start(&sock, on_recv);

  uv_run(&loop, UV_RUN_DEFAULT);
  return 0;
}

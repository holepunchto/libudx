
#include "../include/udx.h"
#include "../src/internal.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// test that multiple small writes are combined into a single packet
// if more than ARRAY_SIZE(pkt->sml_buf) writes are in a single packet, pkt->buf is
// dynamically allocated to hold the iovecs. pkt->buf then is realloc'd as necessary.
uv_loop_t loop;
udx_t udx;

udx_socket_t send_sk;
udx_stream_t send_stream;

udx_socket_t recv_sk;
udx_stream_t recv_stream;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

// NUM_WRITES must be >=     ARRAY_SIZE(pkt->wbuf_sml) to test the malloc path
//        and must be >= 2 * ARRAY_SIZE(pkt->wbuf_sml) to test the realloc path
#define NUM_WRITES (4 * ARRAY_SIZE(send_stream.pkt->wbuf_sml))
#define CHUNK_SIZE 60

void
on_close (udx_stream_t *stream, int status);
void
on_ack (udx_stream_write_t *req, int status, int unordered);
void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf);

int
main () {

  char data[NUM_WRITES * CHUNK_SIZE] = {0};
  struct {
    udx_stream_write_t *write;
    uv_buf_t buf;
  } writes[NUM_WRITES];

  int rc = 0;

  for (int i = 0; i < NUM_WRITES; i++) {
    writes[i].write = malloc(udx_stream_write_sizeof(1));
    writes[i].buf = uv_buf_init(data + i * CHUNK_SIZE, CHUNK_SIZE);
  }

  rc = uv_loop_init(&loop);
  assert(rc == 0);

  rc = udx_init(&loop, &udx, NULL);
  assert(rc == 0);

  rc = udx_socket_init(&udx, &send_sk, NULL);
  assert(rc == 0);

  rc = udx_socket_init(&udx, &recv_sk, NULL);
  assert(rc == 0);

  struct sockaddr_in recv_addr;
  uv_ip4_addr("127.0.0.1", 8082, &recv_addr);
  rc = udx_socket_bind(&recv_sk, (struct sockaddr *) &recv_addr, 0);
  assert(rc == 0);

  struct sockaddr_in send_addr;
  uv_ip4_addr("127.0.0.1", 8081, &send_addr);
  rc = udx_socket_bind(&send_sk, (struct sockaddr *) &send_addr, 0);
  assert(rc == 0);

  rc = udx_stream_init(&udx, &send_stream, 1, on_close, NULL);
  assert(rc == 0);

  assert(udx__max_payload(&send_stream) < (NUM_WRITES * CHUNK_SIZE) && "test data fits in a single packet");

  rc = udx_stream_init(&udx, &recv_stream, 2, on_close, NULL);
  assert(rc == 0);

  rc = udx_stream_connect(&send_stream, &send_sk, 2, (struct sockaddr *) &recv_addr);
  assert(rc == 0);

  rc = udx_stream_connect(&recv_stream, &recv_sk, 1, (struct sockaddr *) &send_addr);
  assert(rc == 0);

  rc = udx_stream_read_start(&recv_stream, on_read);
  assert(rc == 0);

  for (int i = 0; i < NUM_WRITES; i++) {
    rc = udx_stream_write(writes[i].write, &send_stream, &writes[i].buf, 1, on_ack);
    printf("udx_stream_write rc=%d\n", rc);
  }

  udx_stream_write_t *end1 = malloc(udx_stream_write_sizeof(1));
  udx_stream_write_t *end2 = malloc(udx_stream_write_sizeof(1));

  rc = udx_stream_write_end(end1, &send_stream, NULL, 0, NULL);
  assert(rc);
  rc = udx_stream_write_end(end2, &recv_stream, NULL, 0, NULL);
  assert(rc);

  rc = uv_run(&loop, UV_RUN_DEFAULT);
  assert(rc == 0);
  rc = uv_loop_close(&loop);
  assert(rc == 0);

  // free user allocated writes (for valgrind)
  free(end1);
  free(end2);
  for (int i = 0; i < NUM_WRITES; i++) {
    free(writes[i].write);
  }

  // assert(ack_called && read_called && eof_received);
}

int nclosed;

void
on_close (udx_stream_t *stream, int status) {
  assert(status == 0);
  nclosed++;

  printf("on_close: %s\n", stream == &send_stream ? "send_stream" : "recv_stream");
  if (nclosed == 2) {
    udx_socket_close(&send_sk);
    udx_socket_close(&recv_sk);
  }
}
void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  printf("on_ack status=%d int unordered=%d\n", status, unordered);
}
void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len >= 0) {
    printf("on_read: stream=%s read_len=%jd data=%.*s\n", stream == &send_stream ? "send_stream" : "recv_stream", read_len, (int) buf->len, buf->base);
  } else {
    printf("on_read: err=%jd\n", read_len);
  }
}

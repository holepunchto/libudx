#include <assert.h>
#include <stdlib.h>

#include "../include/udx.h"

// MTU promotion is ACK-driven. Keep enough data queued for every search step
// to have a later DATA packet available to carry the next probe.
#define TOTAL_BYTES (8 * 1024 * 1024)

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_stream_t astream;

udx_socket_t bsock;
udx_stream_t bstream;

udx_stream_write_t *req;

int nclosed;

void
on_close (udx_stream_t *stream, int status) {
  assert(status == 0);

  nclosed++;

  if (nclosed == 2) {
    udx_socket_close(&asock);
    udx_socket_close(&bsock);
  }
}

void
on_ack (udx_stream_write_t *r, int status, int unordered) {
  assert(status == 0);

  udx_stream_destroy(&bstream);
  udx_stream_destroy(&astream);
}

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  // A full-write ACK proves that the receiver processed the entire write, so
  // this test only needs reads enabled; data integrity is covered elsewhere.
  (void) handle;
  (void) read_len;
  (void) buf;
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  assert(req != NULL);

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

  e = udx_stream_init(&udx, &astream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  char *data = calloc(1, TOTAL_BYTES);
  assert(data != NULL);

  uv_buf_t buf = uv_buf_init(data, TOTAL_BYTES);
  e = udx_stream_write(req, &bstream, &buf, 1, on_ack);
  assert(e >= 0);

  uv_run(&loop, UV_RUN_DEFAULT);
  e = uv_loop_close(&loop);
  assert(e == 0);

  // Successful in-band MTU probes should promote the sender from the base MTU to the max MTU.
  assert(bstream.mtu == UDX_MTU_MAX);

  free(req);
  free(data);

  return 0;
}

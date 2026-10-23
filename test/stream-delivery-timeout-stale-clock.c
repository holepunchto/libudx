#include <assert.h>
#include <stdlib.h>

#include "../include/udx.h"
#include "../src/internal.h"

// progress_ts is not taken from the stale uv_now after a callback blocks the loop:
// write 2 is sent from the ack callback of write 1, write 3 from idle in a check callback

uv_loop_t loop;
udx_t udx;

udx_socket_t asock;
udx_stream_t astream;

udx_socket_t bsock;
udx_stream_t bstream;

uv_check_t check;

char *data;
size_t data_len;

int acked;
int nclosed;

void
on_close (udx_stream_t *s, int status) {
  assert(status == 0);

  if (++nclosed == 2) {
    udx_socket_close(&asock);
    udx_socket_close(&bsock);
  }
}

void
on_ack (udx_stream_write_t *req, int status, int unordered);

void
write_packet () {
  udx_stream_write_t *req = malloc(udx_stream_write_sizeof(1));
  uv_buf_t buf = uv_buf_init(data, data_len);
  int e = udx_stream_write(req, &astream, &buf, 1, on_ack);
  assert(e);
}

void
on_check (uv_check_t *handle) {
  uv_check_stop(handle);
  write_packet();
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  free(req);

  if (++acked == 3) {
    udx_stream_destroy(&astream);
    udx_stream_destroy(&bstream);
    return;
  }

  uv_sleep(3500);

  if (acked == 1) write_packet();
  else uv_check_start(&check, on_check);
}

void
bind_any (udx_socket_t *socket, struct sockaddr_in *addr) {
  int e = udx_socket_init(&udx, socket, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 0, addr);
  e = udx_socket_bind(socket, (struct sockaddr *) addr, 0);
  assert(e == 0);

  int len = sizeof(*addr);
  e = udx_socket_getsockname(socket, (struct sockaddr *) addr, &len);
  assert(e == 0);
}

int
main () {
  int e;

  uv_loop_init(&loop);
  uv_check_init(&loop, &check);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  struct sockaddr_in aaddr;
  bind_any(&asock, &aaddr);

  struct sockaddr_in baddr;
  bind_any(&bsock, &baddr);

  e = udx_stream_init(&udx, &astream, 1, on_close, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, on_close, NULL);
  assert(e == 0);

  e = udx_stream_connect(&astream, &asock, 2, (struct sockaddr *) &baddr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &bsock, 1, (struct sockaddr *) &aaddr);
  assert(e == 0);

  e = udx_stream_set_delivery_timeout(&astream, 3000);
  assert(e == 0);

  data_len = udx__max_payload(&astream);
  data = calloc(1, data_len);

  write_packet();

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  free(data);

  return 0;
}

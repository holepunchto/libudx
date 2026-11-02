#include "../include/udx.h"
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>

// bare_poll() and Electron run uv_run(UV_RUN_NOWAIT), then sleep for uv_backend_timeout().
// Work that udx defers to a later loop phase must either be done by the time uv_run returns,
// or keep uv_backend_timeout() at 0, otherwise it stalls until something else wakes the loop.
//
// 1. sender: a 1-byte write is a partial packet, deferred to coalesce further writes
// 2. receiver: the ack for a lone data packet is delayed to coalesce with further packets

uv_loop_t loop;
udx_t udx;
struct sockaddr_in addr;
udx_socket_t sock;

udx_stream_t astream;
udx_stream_t bstream;

udx_stream_write_t *req;

bool read_called = false;

void
on_read (udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf) {
  read_called = true;
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock, NULL);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 18151, &addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &astream, 1, NULL, NULL);
  assert(e == 0);

  e = udx_stream_init(&udx, &bstream, 2, NULL, NULL);
  assert(e == 0);

  e = udx_stream_read_start(&astream, on_read);
  assert(e == 0);

  e = udx_stream_connect(&astream, &sock, 2, (struct sockaddr *) &addr);
  assert(e == 0);

  e = udx_stream_connect(&bstream, &sock, 1, (struct sockaddr *) &addr);
  assert(e == 0);

  // let the loop settle so that it would sleep, otherwise the checks below prove nothing
  for (int i = 0; i < 100 && uv_backend_timeout(&loop) == 0; i++) {
    uv_run(&loop, UV_RUN_NOWAIT);
  }
  assert(uv_backend_timeout(&loop) != 0);

  // the write happens outside uv_run, as it does when called from a host's JS between runs
  uv_buf_t buf = uv_buf_init("a", 1);
  e = udx_stream_write(req, &bstream, &buf, 1, NULL);
  assert(e >= 0);

  assert(bstream.packets_tx == 1 || uv_backend_timeout(&loop) == 0);

  while (!read_called) {
    uv_run(&loop, UV_RUN_NOWAIT);
  }

  assert(astream.packets_tx == 1 || uv_backend_timeout(&loop) == 0);

  udx_stream_destroy(&astream);
  udx_stream_destroy(&bstream);
  udx_socket_close(&sock);

  uv_run(&loop, UV_RUN_DEFAULT);

  free(req);

  return 0;
}

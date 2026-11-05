#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"

// like stream-write-read-receive-window-2, but the application calls
// udx_stream_window_update after consuming data. the sender should learn the
// window reopened from the window update ack instead of a zero window probe.

uv_loop_t loop;
udx_t udx;

udx_socket_t recv_sock;
udx_stream_t recv_stream;

udx_socket_t send_sock;
udx_stream_t send_stream;

udx_stream_write_t *req;

uv_timer_t application_timer;

#define APPLICATION_BUFFER_SIZE 2000

struct {
  size_t nbytes_in_buffer;
} application;

int nwindow_updates;

void
app_tick (uv_timer_t *timer) {
  if (application.nbytes_in_buffer < 500) {
    application.nbytes_in_buffer = 0;
  } else {
    application.nbytes_in_buffer -= 500;
  }

  nwindow_updates += udx_stream_window_update(&recv_stream);
}

uint32_t
application_get_buffer (udx_stream_t *stream) {
  return application.nbytes_in_buffer;
}

void
init_application () {
  uv_timer_init(&loop, &application_timer);
  uv_timer_start(&application_timer, app_tick, 0, 100);
  application.nbytes_in_buffer = 0;
  udx_stream_set_rwnd_max(&recv_stream, APPLICATION_BUFFER_SIZE);
  recv_stream.get_read_buffer_size = application_get_buffer;
}

void
application_on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len == UV_EOF) {
    uv_timer_stop(&application_timer);
    uv_close((uv_handle_t *) &application_timer, NULL);
  }

  if (read_len > 0) {
    application.nbytes_in_buffer += read_len;
  }
}

udx_stream_write_t *send_end_req;
udx_stream_write_t *recv_end_req;

int nend;
int nstream_close;
int nsocket_close;
int nfinalize;

bool ack_called;

void
on_end (udx_stream_write_t *req, int status, int unordered) {
  nend++;
}

void
on_close (udx_stream_t *stream, int status) {
  nstream_close++;
}

void
on_socket_close (udx_socket_t *s) {
  nsocket_close++;
}

void
on_finalize (udx_stream_t *stream) {
  nfinalize++;
  if (nfinalize == 2) {
    udx_socket_close(&send_sock);
    udx_socket_close(&recv_sock);
  }
}

void
on_ack (udx_stream_write_t *req, int status, int unordered) {
  assert(status == 0);
  assert(unordered == 0);

  ack_called = true;
  send_end_req = malloc(udx_stream_write_sizeof(1));
  recv_end_req = malloc(udx_stream_write_sizeof(1));

  udx_stream_write_end(send_end_req, &send_stream, NULL, 0, on_end);
  udx_stream_write_end(recv_end_req, &recv_stream, NULL, 0, on_end);
}

int
main () {
  int e;

  req = malloc(udx_stream_write_sizeof(1));

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &recv_sock, on_socket_close);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, on_socket_close);
  assert(e == 0);

  struct sockaddr_in send_addr;
  uv_ip4_addr("127.0.0.1", 8082, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  struct sockaddr_in recv_addr;
  uv_ip4_addr("127.0.0.1", 8081, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &recv_stream, 1, on_close, on_finalize);
  assert(e == 0);

  e = udx_stream_init(&udx, &send_stream, 2, on_close, on_finalize);
  assert(e == 0);

  // not connected yet, nothing to update
  assert(udx_stream_window_update(&recv_stream) == 0);

  init_application();
  send_stream.send_rwnd = APPLICATION_BUFFER_SIZE;
  assert(recv_stream.rto == 1000);

  e = udx_stream_connect(&recv_stream, &recv_sock, 2, (struct sockaddr *) &send_addr);
  assert(e == 0);

  e = udx_stream_connect(&send_stream, &send_sock, 1, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_stream_read_start(&recv_stream, application_on_read);
  assert(e == 0);

  int data_sz = UDX_MTU_MAX * 6;
  char *data = malloc(data_sz);
  uv_buf_t buf = uv_buf_init(data, data_sz);

  e = udx_stream_write(req, &send_stream, &buf, 1, on_ack);
  assert(e && "drained");

  uint64_t start = uv_now(&loop);

  uv_run(&loop, UV_RUN_DEFAULT);

  uint64_t elapsed = uv_now(&loop) - start;

  // the application drains a full window in 400ms, well under the 1000ms rto
  // that arms the zero window probe, so every reopening is announced by a
  // window update and the sender never needs to probe.

  assert(nwindow_updates > 0);
  assert(send_stream.zwp_count == 0);
  assert(recv_stream.zwp_count == 0);
  assert(elapsed < 5000);
  assert(ack_called);
  assert(send_stream.rto_count == 0);
  assert(recv_stream.retransmit_count == 0);
  assert(nend == 2 && nstream_close == 2 && nfinalize == 2 && nsocket_close == 2);
  free(data);

  assert(send_stream.bytes_queued == buf.len);
  assert(send_stream.bytes_packetized == buf.len);
  assert(send_stream.bytes_acked == buf.len);

  return 0;
}

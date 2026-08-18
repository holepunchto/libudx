#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/udx.h"
#include "../src/endian.h"

#define LOCAL_ID            1
#define OLD_REMOTE_ID       2
#define NEW_REMOTE_ID       3
#define IPV4_NETWORK_HEADER (UDX_IPV4_HEADER_SIZE - UDX_HEADER_SIZE)
#define BASE_DATA_SIZE      (UDX_MTU_BASE - UDX_IPV4_HEADER_SIZE)
#define MAX_DATA_SIZE       (UDX_MTU_MAX - UDX_IPV4_HEADER_SIZE)
#define WRITE_SIZE          (BASE_DATA_SIZE + 1)

uv_loop_t loop;
uv_timer_t watchdog;
udx_t udx;

struct sockaddr_in send_addr;
udx_socket_t send_socket;
udx_stream_t send_stream;

struct sockaddr_in old_addr;
udx_socket_t old_socket;

struct sockaddr_in new_addr;
udx_socket_t new_socket;

udx_stream_write_t *old_write;
udx_stream_write_t *new_write;
char data[WRITE_SIZE];

udx_socket_send_t ack_req;
uint8_t ack_header[UDX_HEADER_SIZE];

bool old_ack_sent;
size_t new_bytes_received;

static uint32_t
read_uint32 (const uint8_t *buf) {
  uint32_t value;
  memcpy(&value, buf, sizeof(value));
  return udx__swap_uint32_if_be(value);
}

static void
write_uint32 (uint8_t *buf, uint32_t value) {
  value = udx__swap_uint32_if_be(value);
  memcpy(buf, &value, sizeof(value));
}

static void
bind_ephemeral (udx_socket_t *socket, struct sockaddr_in *addr) {
  int err = uv_ip4_addr("127.0.0.1", 0, addr);
  assert(err == 0);

  err = udx_socket_bind(socket, (struct sockaddr *) addr, 0);
  assert(err == 0);

  int addr_len = sizeof(*addr);
  err = udx_socket_getsockname(socket, (struct sockaddr *) addr, &addr_len);
  assert(err == 0);
}

static void
send_old_ack () {
  ack_header[0] = UDX_MAGIC_BYTE;
  ack_header[1] = UDX_VERSION;

  write_uint32(ack_header + 4, LOCAL_ID);
  write_uint32(ack_header + 8, UINT32_MAX);
  write_uint32(ack_header + 12, 1);
  write_uint32(ack_header + 16, 1);

  uv_buf_t buf = uv_buf_init((char *) ack_header, sizeof(ack_header));
  int err = udx_socket_send(&ack_req, &old_socket, &buf, 1, (struct sockaddr *) &send_addr, NULL);
  assert(err == 0);
}

static void
on_recv (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) from;

  assert(read_len >= UDX_HEADER_SIZE);

  const uint8_t *header = (const uint8_t *) buf->base;
  assert(header[0] == UDX_MAGIC_BYTE && header[1] == UDX_VERSION);

  if (!(header[2] & UDX_HEADER_DATA)) return;

  const uint8_t data_offset = header[3];
  assert((size_t) read_len >= UDX_HEADER_SIZE + data_offset);

  const uint32_t remote_id = read_uint32(header + 4);
  const size_t payload_size = read_len - UDX_HEADER_SIZE - data_offset;

  if (socket == &old_socket) {
    assert(remote_id == OLD_REMOTE_ID);
    assert(payload_size == WRITE_SIZE);

    if (!old_ack_sent) {
      old_ack_sent = true;
      send_old_ack();
    }
    return;
  }

  assert(remote_id == NEW_REMOTE_ID);
  assert(read_len + IPV4_NETWORK_HEADER <= UDX_MTU_BASE);

  new_bytes_received += payload_size;
  assert(new_bytes_received <= WRITE_SIZE);

  if (new_bytes_received == WRITE_SIZE) {
    uv_timer_stop(&watchdog);
    uv_close((uv_handle_t *) &watchdog, NULL);

    int err = udx_stream_destroy(&send_stream);
    assert(err == 1);
  }
}

static void
on_remote_changed (udx_stream_t *stream) {
  uv_buf_t buf = uv_buf_init(data, sizeof(data));
  int err = udx_stream_write(new_write, stream, &buf, 1, NULL);
  assert(err >= 0);
}

static void
on_stream_close (udx_stream_t *stream, int status) {
  (void) stream;
  assert(status == 0);

  int err = udx_socket_close(&send_socket);
  assert(err == 0);
  err = udx_socket_close(&old_socket);
  assert(err == 0);
  err = udx_socket_close(&new_socket);
  assert(err == 0);
}

static void
on_watchdog (uv_timer_t *timer) {
  (void) timer;
  assert(false && "timed out waiting for remote-change MTU regression test");
}

int
main () {
  int err = uv_loop_init(&loop);
  assert(err == 0);

  err = udx_init(&loop, &udx, NULL);
  assert(err == 0);

  err = udx_socket_init(&udx, &send_socket, NULL);
  assert(err == 0);
  err = udx_socket_init(&udx, &old_socket, NULL);
  assert(err == 0);
  err = udx_socket_init(&udx, &new_socket, NULL);
  assert(err == 0);

  bind_ephemeral(&send_socket, &send_addr);
  bind_ephemeral(&old_socket, &old_addr);
  bind_ephemeral(&new_socket, &new_addr);

  err = udx_socket_recv_start(&old_socket, on_recv);
  assert(err == 0);
  err = udx_socket_recv_start(&new_socket, on_recv);
  assert(err == 0);

  err = udx_stream_init(&udx, &send_stream, LOCAL_ID, on_stream_close, NULL);
  assert(err == 0);
  err = udx_stream_connect(&send_stream, &send_socket, NEW_REMOTE_ID, (struct sockaddr *) &new_addr);
  assert(err == 0);

  // An empty packet builder must adopt the new MTU immediately.
  send_stream.mtu = UDX_MTU_MAX;
  send_stream.pkt_capacity = MAX_DATA_SIZE;

  err = udx_stream_change_remote(&send_stream, &send_socket, OLD_REMOTE_ID, (struct sockaddr *) &old_addr, NULL);
  assert(err == 1);
  assert(send_stream.pkt_capacity == BASE_DATA_SIZE);

  // Leave a packet that fits the promoted old path, but not the new base MTU,
  // partially constructed across the remote change.
  send_stream.mtu = UDX_MTU_MAX;
  send_stream.pkt_capacity = MAX_DATA_SIZE;

  old_write = malloc(udx_stream_write_sizeof(1));
  new_write = malloc(udx_stream_write_sizeof(1));
  assert(old_write != NULL && new_write != NULL);

  uv_buf_t buf = uv_buf_init(data, sizeof(data));
  err = udx_stream_write(old_write, &send_stream, &buf, 1, NULL);
  assert(err >= 0);

  err = udx_stream_change_remote(&send_stream, &send_socket, NEW_REMOTE_ID, (struct sockaddr *) &new_addr, on_remote_changed);
  assert(err == 0);
  assert(send_stream.pkt_capacity == MAX_DATA_SIZE - WRITE_SIZE);

  err = uv_timer_init(&loop, &watchdog);
  assert(err == 0);
  err = uv_timer_start(&watchdog, on_watchdog, 10000, 0);
  assert(err == 0);

  uv_run(&loop, UV_RUN_DEFAULT);

  err = uv_loop_close(&loop);
  assert(err == 0);

  free(old_write);
  free(new_write);

  return 0;
}

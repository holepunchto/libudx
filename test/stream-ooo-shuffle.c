#include "../include/udx.h"
#include "../src/endian.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
// this test writes ~1000 1-byte packets in random order
// and asserts that the stream receives the original message in order

uv_loop_t loop;
udx_t udx;

udx_socket_t send_sock;
udx_socket_t recv_sock;
udx_stream_t stream;

struct sockaddr_in send_addr;
struct sockaddr_in recv_addr;

char message[] = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";

int sequence[sizeof(message) - 1];

char recv_buffer[sizeof(message) - 1];
int recv_offset;

static void
on_socket_close (udx_socket_t *socket) {
  (void) socket;
}

void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len < 0) {
    udx_stream_destroy(stream);
    return;
  }

  assert(read_len == buf->len);

  memcpy(recv_buffer + recv_offset, buf->base, buf->len);
  recv_offset += read_len;
}

static void
on_stream_close (udx_stream_t *s, int status) {
  (void) s;
  (void) status;

  udx_socket_close(&send_sock);
  udx_socket_close(&recv_sock);
}

int acks_received;

// if the socket doesn't have a stream, on_recv will pick up the packet
static void
on_recv (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  acks_received++;
}

int
main (int argc, char **argv) {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &send_sock, on_socket_close);
  assert(e == 0);

  e = udx_socket_init(&udx, &recv_sock, on_socket_close);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 0, &send_addr);
  e = udx_socket_bind(&send_sock, (struct sockaddr *) &send_addr, 0);
  assert(e == 0);

  int send_addr_sz = sizeof(send_addr);
  e = udx_socket_getsockname(&send_sock, (struct sockaddr *) &send_addr, &send_addr_sz);
  assert(e == 0);

  uv_ip4_addr("127.0.0.1", 0, &recv_addr);
  e = udx_socket_bind(&recv_sock, (struct sockaddr *) &recv_addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);

  int recv_addr_sz = sizeof(recv_addr);
  e = udx_socket_getsockname(&recv_sock, (struct sockaddr *) &recv_addr, &recv_addr_sz);
  assert(e == 0);

  e = udx_stream_connect(&stream, &send_sock, 2, (struct sockaddr *) &recv_addr);
  assert(e == 0);

  e = udx_socket_recv_start(&recv_sock, on_recv);
  assert(e == 0);

  e = udx_stream_read_start(&stream, on_read);
  assert(e == 0);

  int npackets = sizeof(message) - 1;

  assert(npackets == strlen(message));

  // in order
  for (int i = 0; i < npackets; i++) {
    sequence[i] = i;
  }
  // shuffle
  for (int i = npackets - 1; i > 0 /* stop before 0 */; i--) {
    int j = rand() % i;
    int tmp = sequence[j];
    sequence[j] = sequence[i];
    sequence[i] = tmp;
  }

  struct {
    uint8_t magic;
    uint8_t version;
    uint8_t type;
    uint8_t data_offset;
    uint32_t id;
    uint32_t rwnd;
    uint32_t seq;
    uint32_t ack;
  } hdr;

  hdr.magic = 0xff;
  hdr.version = 1;
  hdr.type = UDX_HEADER_DATA;
  hdr.data_offset = 0;
  hdr.id = udx__swap_uint32_if_be(1);
  hdr.rwnd = udx__swap_uint32_if_be(0x100000);
  hdr.seq = 0; // fill in
  hdr.ack = 0;

  hdr.seq = udx__swap_uint32_if_be(npackets);
  hdr.type = UDX_HEADER_END;
  uv_buf_t b = uv_buf_init((char *) &hdr, sizeof(hdr));

  int rc = uv_udp_try_send(&recv_sock.uv_udp, &b, 1, (struct sockaddr *) &send_addr);
  assert(rc == b.len);
  hdr.type = UDX_HEADER_DATA;

  for (int i = 0; i < npackets; i++) {
    hdr.seq = udx__swap_uint32_if_be(sequence[i]);
    uv_buf_t b[2];
    b[0] = uv_buf_init((char *) &hdr, sizeof(hdr));
    b[1] = uv_buf_init(&message[sequence[i]], 1);
    uv_udp_try_send(&recv_sock.uv_udp, b, 2, (struct sockaddr *) &send_addr);
  }
  // for fun, we send the end packet first

  // hdr.seq = udx__swap_uint32_if_be(npackets);
  // hdr.type = UDX_HEADER_END;
  // uv_buf_t b = uv_buf_init((char *) &hdr, sizeof(hdr));

  // uv_udp_try_send(&recv_sock.uv_udp, &b, 1, (struct sockaddr *) &send_addr);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);

  e = uv_loop_close(&loop);
  assert(e == 0);

  assert(memcmp(message, recv_buffer, sizeof(message) - 1) == 0);

  return 0;
}

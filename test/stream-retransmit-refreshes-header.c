#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "../include/udx.h"
#include "../src/endian.h"

uv_loop_t loop;
udx_t udx;
udx_socket_t sender;
udx_socket_t receiver;
udx_stream_t stream;
udx_stream_write_t *write_req;

int data_packets = 0;
uint32_t first_ack = 0;
uint32_t first_rwnd = 0;
uint32_t retransmit_ack = 0;
uint32_t retransmit_rwnd = 0;

static void
on_socket_close (udx_socket_t *socket) {
  (void) socket;
}

static void
on_stream_close (udx_stream_t *stream, int status) {
  (void) stream;
  assert(status == 0);
  udx_socket_close(&sender);
  udx_socket_close(&receiver);
}

static void
on_recv (udx_socket_t *socket, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from) {
  (void) socket;
  (void) from;

  assert(read_len >= UDX_HEADER_SIZE);
  if (!((uint8_t) buf->base[2] & UDX_HEADER_DATA)) return;

  uint32_t rwnd = udx__swap_uint32_if_be(*(uint32_t *) (buf->base + 8));
  uint32_t ack = udx__swap_uint32_if_be(*(uint32_t *) (buf->base + 16));

  if (++data_packets == 1) {
    first_rwnd = rwnd;
    first_ack = ack;

    // Model reverse data arriving after this packet was created. With its
    // standalone ACK lost, the retransmission must carry the new state.
    int e = udx_stream_set_ack(&stream, 5);
    assert(e == 0);
    e = udx_stream_set_rwnd_max(&stream, 1024);
    assert(e == 0);
    return;
  }

  retransmit_rwnd = rwnd;
  retransmit_ack = ack;
  udx_stream_destroy(&stream);
}

int
main () {
  int e = uv_loop_init(&loop);
  assert(e == 0);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);
  e = udx_socket_init(&udx, &sender, on_socket_close);
  assert(e == 0);
  e = udx_socket_init(&udx, &receiver, on_socket_close);
  assert(e == 0);

  struct sockaddr_in sender_addr;
  struct sockaddr_in receiver_addr;
  uv_ip4_addr("127.0.0.1", 18111, &sender_addr);
  uv_ip4_addr("127.0.0.1", 18112, &receiver_addr);
  e = udx_socket_bind(&sender, (struct sockaddr *) &sender_addr, 0);
  assert(e == 0);
  e = udx_socket_bind(&receiver, (struct sockaddr *) &receiver_addr, 0);
  assert(e == 0);
  e = udx_socket_recv_start(&receiver, on_recv);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_stream_close, NULL);
  assert(e == 0);
  e = udx_stream_connect(&stream, &sender, 2, (struct sockaddr *) &receiver_addr);
  assert(e == 0);
  stream.rto = 20;

  write_req = malloc(udx_stream_write_sizeof(1));
  assert(write_req != NULL);
  uv_buf_t data = uv_buf_init("data", 4);
  e = udx_stream_write(write_req, &stream, &data, 1, NULL);
  assert(e != 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  e = uv_loop_close(&loop);
  assert(e == 0);

  assert(data_packets >= 2);
  assert(first_ack == 0);
  assert(first_rwnd != 1024);
  assert(retransmit_ack == 5);
  assert(retransmit_rwnd == 1024);

  free(write_req);
  return 0;
}

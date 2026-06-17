
#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// send a badly formed DATA+SACK packet and assert that we do NOT get a READ event
// for it (ie it was dropped due to the bad sack)

#include "../include/udx.h"
#include "../src/endian.h"

uv_loop_t loop;
udx_t udx;

udx_socket_t sock;
udx_stream_t stream;

udx_socket_send_t req0; // DATA+SACK (dropped)
udx_socket_send_t req1; // END

int nread;

void
on_read (udx_stream_t *stream, ssize_t read_len, const uv_buf_t *buf) {
  if (read_len == UV_EOF) {
    udx_stream_destroy(stream);
  }
  nread++;
}

void
on_socket_send (udx_socket_send_t *req, int status) {
  assert(status == 0);
}

void
on_close (udx_stream_t *s, int status) {
  udx_socket_close(&sock);
}

int
main () {
  int e;

  uv_loop_init(&loop);

  e = udx_init(&loop, &udx, NULL);
  assert(e == 0);

  e = udx_socket_init(&udx, &sock, NULL);
  assert(e == 0);

  struct sockaddr_in addr;
  uv_ip4_addr("127.0.0.1", 8081, &addr);
  e = udx_socket_bind(&sock, (struct sockaddr *) &addr, 0);
  assert(e == 0);

  e = udx_stream_init(&udx, &stream, 1, on_close, NULL);
  assert(e == 0);

  // due to a quirk, we don't send the user's read(EOF) event until
  // the END packet has been acked, and we defer sending ACK on unconnected
  // streams until they are connected.
  // therefore we connect to a dummy stream to send the ack to it.
  e = udx_stream_connect(&stream, &sock, 0, (struct sockaddr *) &addr);
  assert(e == 0);

  e = udx_stream_read_start(&stream, on_read);
  assert(e == 0);

  struct {
    struct {
      uint8_t magic;
      uint8_t version;
      uint8_t type;
      uint8_t data_offset;
      uint32_t remote_id;
      uint32_t rwnd;
      uint32_t seq;
      uint32_t ack;
    } hdr;
    union {
      uint8_t payload[1024];
      struct {
        uint32_t start;
        uint32_t end;
      } sack[128];
    } u;
  } pkt;

  // forge a data packet with a bad sack block
  memset(&pkt, 0, sizeof(pkt));
  pkt.hdr.magic = 0xff;
  pkt.hdr.version = 1;
  pkt.hdr.type = UDX_HEADER_DATA | UDX_HEADER_SACK;
  pkt.hdr.data_offset = 8;
  pkt.hdr.remote_id = udx__swap_uint32_if_be(1);
  pkt.hdr.rwnd = udx__swap_uint32_if_be(UINT32_MAX);
  pkt.u.sack[0].start = udx__swap_uint32_if_be(1);
  pkt.u.sack[0].end = udx__swap_uint32_if_be(0);

  uv_buf_t buf = uv_buf_init((char *) &pkt, sizeof(pkt));

  e = udx_socket_send(&req0, &sock, &buf, 1, (struct sockaddr *) &addr, on_socket_send);
  assert(e == 0);

  // send an END to trigger the end of the test
  pkt.hdr.type = UDX_HEADER_END;
  pkt.hdr.seq++;
  pkt.hdr.data_offset = 0;
  buf = uv_buf_init((char *) &pkt, sizeof(pkt.hdr));

  e = udx_socket_send(&req1, &sock, &buf, 1, (struct sockaddr *) &addr, on_socket_send);
  assert(e == 0);

  e = uv_run(&loop, UV_RUN_DEFAULT);
  assert(e == 0);
  e = uv_loop_close(&loop);
  assert(e == 0);

  assert(nread == 2);
  assert(stream.dropped_sacks == 1);

  return 0;
}

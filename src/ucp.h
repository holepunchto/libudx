#ifndef UCP_H
#define UCP_H

#include "fifo.h"
#include "cirbuf.h"
#include <stdint.h>
#include <string.h>
#include <uv.h>

#define UCP_PACKET_SIZE 1435
#define UCP_HEADER_SIZE 16
#define UCP_MAX_PACKET_DATA (UCP_PACKET_SIZE - UCP_HEADER_SIZE)

typedef struct ucp {
  uv_udp_t handle;
  uv_poll_t io_poll;
  uv_loop_t *loop;
  ucp_fifo_t send_queue;

  int bound;
  void *userdata;

  struct sockaddr_in on_message_addr;
  void (*on_message)(struct ucp *self, const char *buf, ssize_t buf_len, const struct sockaddr *from);

  uint32_t active_sockets;
  ucp_cirbuf_t sockets;
} ucp_t;

typedef struct {
  uint32_t seq;
  uint8_t transmits;
  uint8_t queued;

  struct msghdr h;
  struct ucp_write *write;
  struct ucp_send *send;

  // just alloc it in place here, easier to manage
  char header[UCP_HEADER_SIZE];
  struct iovec buf[2];
} ucp_outgoing_packet_t;

typedef struct {
  uint32_t seq;

  struct iovec buf;
} ucp_incoming_packet_t;

typedef struct ucp_write {
  uint32_t packets;
  void *userdata;
} ucp_write_t;

typedef struct ucp_send {
  ucp_outgoing_packet_t pkt;
  struct sockaddr dest;
  void *userdata;
} ucp_send_t;

typedef struct ucp_stream {
  uint32_t local_id;
  uint32_t remote_id;

  ucp_t *ucp;

  struct sockaddr remote_addr;

  void *userdata;

  void (*on_read)(struct ucp_stream *stream, const char *buf, size_t buf_len);
  void (*on_write)(struct ucp_stream *stream, ucp_write_t *req, int status);

  uint32_t seq;
  uint32_t ack;
  uint32_t remote_acked;

  ucp_cirbuf_t outgoing;
  ucp_cirbuf_t incoming;
} ucp_stream_t;

enum UCP_TYPE {
  UCP_ON_MESSAGE = 1,
  UCP_ON_READ = 2,
  UCP_ON_WRITE = 3,
};

int
ucp_init (ucp_t *self, uv_loop_t *loop);

int
ucp_set_callback(ucp_t *self, enum UCP_TYPE name, void *fn);

int
ucp_send_buffer_size(ucp_t *self, int *value);

int
ucp_recv_buffer_size(ucp_t *self, int *value);

int
ucp_bind (ucp_t *self, const struct sockaddr *addr);

int
ucp_send (ucp_t *self, ucp_send_t *req, const char *buf, size_t buf_len, const struct sockaddr *addr);

int
ucp_stream_init (ucp_t *self, ucp_stream_t *stream);

int
ucp_stream_set_callback(ucp_stream_t *stream, enum UCP_TYPE name, void *fn);

void
ucp_stream_connect (ucp_stream_t *stream, uint32_t remote_id, const struct sockaddr *remote_addr);

int
ucp_stream_resend (ucp_stream_t *stream);

int
ucp_stream_write (ucp_stream_t *stream, ucp_write_t *req, const char *buf, size_t buf_len);

int
ucp_stream_send_state (ucp_stream_t *stream);

#endif

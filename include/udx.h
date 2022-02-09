#ifndef UDX_H
#define UDX_H

#include <stdint.h>
#include <string.h>
#include <uv.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "udx/fifo.h"
#include "udx/cirbuf.h"
#include "udx/utils.h"

// TODO: research the packets sizes a bit more
#define UDX_MTU 1400
#define UDX_HEADER_SIZE 20
#define UDX_MAX_DATA_SIZE (UDX_MTU - UDX_HEADER_SIZE)

#define UDX_CLOCK_GRANULARITY_MS 20

#define UDX_MAGIC_BYTE 255
#define UDX_VERSION 1

#define UDX_SOCKET_READING 0b01
#define UDX_SOCKET_BOUND   0b10
#define UDX_SOCKET_PAUSED  0b10 // ~READING truncated to bits

#define UDX_STREAM_CONNECTED        0b000000001
#define UDX_STREAM_ENDING           0b000000010
#define UDX_STREAM_ENDING_REMOTE    0b000000100
#define UDX_STREAM_ENDED            0b000001000
#define UDX_STREAM_ENDED_REMOTE     0b000010000
#define UDX_STREAM_DESTROYING       0b000100000
#define UDX_STREAM_DESTROYED        0b001000000
#define UDX_STREAM_DESTROYED_REMOTE 0b010000000
#define UDX_STREAM_CLOSED           0b100000000

#define UDX_PACKET_WAITING  1
#define UDX_PACKET_SENDING  2
#define UDX_PACKET_INFLIGHT 3

#define UDX_PACKET_STREAM_STATE   0b00001
#define UDX_PACKET_STREAM_WRITE   0b00010
#define UDX_PACKET_STREAM_SEND    0b00100
#define UDX_PACKET_STREAM_DESTROY 0b01000
#define UDX_PACKET_SEND           0b10000

#define UDX_HEADER_DATA    0b00001
#define UDX_HEADER_END     0b00010
#define UDX_HEADER_SACK    0b00100
#define UDX_HEADER_MESSAGE 0b01000
#define UDX_HEADER_DESTROY 0b10000

#define UDX_ERROR_DESTROYED        -1
#define UDX_ERROR_DESTROYED_REMOTE -2
#define UDX_ERROR_TIMEOUT          -3

// declare these upfront to avoid circular deps.

struct udx_write;
struct udx_send;
struct udx_stream_send;
struct udx_stream;

typedef struct udx {
  uv_udp_t handle;
  uv_poll_t io_poll;
  uv_loop_t *loop;
  udx_fifo_t send_queue;
  uv_timer_t timer;

  int status;
  int readers;
  int events;
  int pending_closes;

  void *data;

  struct sockaddr_in on_message_addr;

  void (*on_send)(struct udx *self, struct udx_send *req, int failed);
  void (*on_message)(struct udx *self, const char *buf, size_t buf_len, const struct sockaddr *from);
  void (*on_close)(struct udx *self);

  uint32_t streams_len;
  uint32_t streams_max_len;
  struct udx_stream **streams;

  udx_cirbuf_t streams_by_id;
} udx_t;

typedef struct {
  uint32_t seq; // must be the first entry, so its compat with the cirbuf

  int status;
  int type;

  uint32_t fifo_gc;

  uint8_t transmits;
  uint16_t size;
  uint64_t time_sent;

  void *ctx;

  struct msghdr h;

  // just alloc it in place here, easier to manage
  char header[UDX_HEADER_SIZE];
  struct iovec buf[2];
} udx_packet_t;

typedef struct {
  uint32_t seq; // must be the first entry, so its compat with the cirbuf

  int type;

  struct iovec buf;
} udx_pending_read_t;

typedef struct udx_write {
  uint32_t packets;
  struct udx_stream *stream;

  void *data;
} udx_write_t;

typedef struct udx_send {
  udx_packet_t pkt;
  struct sockaddr dest;

  void *data;
} udx_send_t;

typedef struct udx_stream_send {
  udx_packet_t pkt;
  struct udx_stream *stream;

  void *data;
} udx_stream_send_t;

typedef struct udx_stream {
  uint32_t local_id; // must be first entry, so its compat with the cirbuf
  uint32_t remote_id;

  int set_id;
  int status;

  udx_t *udx;

  struct sockaddr remote_addr;

  void *data;

  void (*on_data)(struct udx_stream *stream, const char *buf, size_t buf_len);
  void (*on_end)(struct udx_stream *stream);
  void (*on_drain)(struct udx_stream *stream);
  void (*on_ack)(struct udx_stream *stream, udx_write_t *req, int failed, int unordered);
  void (*on_send)(struct udx_stream *stream, struct udx_stream_send *req, int failed);
  void (*on_message)(struct udx_stream *stream, const char *buf, size_t buf_len);
  void (*on_close)(struct udx_stream *stream, int hard_close);

  uint32_t seq;
  uint32_t ack;
  uint32_t remote_acked;
  uint32_t remote_ended;

  uint32_t srtt;
  uint32_t rttvar;
  uint32_t rto;

  uint64_t rto_timeout;

  uint32_t pkts_waiting;
  uint32_t pkts_inflight;
  uint32_t dup_acks;
  uint32_t retransmits_waiting;

  size_t inflight;
  size_t ssthresh;
  size_t cwnd;
  size_t rwnd;

  size_t stats_sacks;
  size_t stats_pkts_sent;
  size_t stats_fast_rt;
  uint32_t stats_last_seq;

  udx_cirbuf_t outgoing;
  udx_cirbuf_t incoming;
} udx_stream_t;

typedef void (*udx_send_cb)(udx_t *self, udx_send_t *req, int failed);

typedef void (*udx_message_cb)(udx_t *self, const char *buf, size_t buf_len, const struct sockaddr *from);

typedef void (*udx_close_cb)(udx_t *self);

typedef void (*udx_stream_data_cb)(udx_stream_t *stream, const char *buf, size_t buf_len);

typedef void (*udx_stream_end_cb)(udx_stream_t *stream);

typedef void (*udx_stream_drain_cb)(udx_stream_t *stream);

typedef void (*udx_stream_ack_cb)(udx_stream_t *stream, udx_write_t *req, int failed, int unordered);

typedef void (*udx_stream_send_cb)(udx_stream_t *stream, udx_stream_send_t *req, int failed);

typedef void (*udx_stream_message_cb)(udx_stream_t *stream, const char *buf, size_t buf_len);

typedef void (*udx_stream_close_cb)(udx_stream_t *stream, int hard_close);

int
udx_init (udx_t *self, uv_loop_t *loop);

void
udx_set_on_send(udx_t *self, udx_send_cb cb);

void
udx_set_on_message(udx_t *self, udx_message_cb cb);

void
udx_set_on_close(udx_t *self, udx_close_cb cb);

int
udx_send_buffer_size(udx_t *self, int *value);

int
udx_recv_buffer_size(udx_t *self, int *value);

int
udx_set_ttl(udx_t *self, int ttl);

int
udx_bind (udx_t *self, const struct sockaddr *addr);

int
udx_getsockname (udx_t *self, struct sockaddr * name, int *name_len);

int
udx_send (udx_t *self, udx_send_t *req, const char *buf, size_t buf_len, const struct sockaddr *addr);

int
udx_read_start (udx_t *self);

int
udx_read_stop (udx_t *self);

int
udx_close (udx_t *self);

// only exposed here as a convenience / debug tool - the udx instance uses this automatically
int
udx_check_timeouts (udx_t *self);

int
udx_stream_init (udx_t *self, udx_stream_t *stream, uint32_t *local_id);

void
udx_stream_set_on_data(udx_stream_t *stream, udx_stream_data_cb cb);

void
udx_stream_set_on_end(udx_stream_t *stream, udx_stream_end_cb cb);

void
udx_stream_set_on_drain(udx_stream_t *stream, udx_stream_drain_cb cb);

void
udx_stream_set_on_ack(udx_stream_t *stream, udx_stream_ack_cb cb);

void
udx_stream_set_on_send(udx_stream_t *stream, udx_stream_send_cb cb);

void
udx_stream_set_on_message(udx_stream_t *stream, udx_stream_message_cb cb);

void
udx_stream_set_on_close(udx_stream_t *stream, udx_stream_close_cb cb);

void
udx_stream_connect (udx_stream_t *stream, uint32_t remote_id, const struct sockaddr *remote_addr);

// only exposed here as a convenience / debug tool - the udx instance uses this automatically
int
udx_stream_check_timeouts (udx_stream_t *stream);

int
udx_stream_send (udx_stream_t *stream, udx_stream_send_t *req, const char *buf, size_t buf_len);

int
udx_stream_write (udx_stream_t *stream, udx_write_t *req, const char *buf, size_t buf_len);

int
udx_stream_end (udx_stream_t *stream, udx_write_t *req);

int
udx_stream_destroy (udx_stream_t *stream);

#ifdef __cplusplus
}
#endif
#endif // UDX_H

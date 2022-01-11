#ifndef UCP_H
#define UCP_H

#include "fifo.h"
#include "cirbuf.h"
#include <stdint.h>
#include <string.h>
#include <uv.h>

// TODO: research the packets sizes a bit more
#define UCP_MTU 1400
#define UCP_HEADER_SIZE 20
#define UCP_MAX_DATA_SIZE (UCP_MTU - UCP_HEADER_SIZE)

#define UCP_CLOCK_GRANULARITY_MS 20

#define UCP_MAGIC_BYTE 255
#define UCP_VERSION 1

#define UCP_SOCKET_READING 0b01
#define UCP_SOCKET_BOUND   0b10
#define UCP_SOCKET_PAUSED  0b10 // ~READING truncated to bits

#define UCP_STREAM_CONNECTED        0b000000001
#define UCP_STREAM_ENDING           0b000000010
#define UCP_STREAM_ENDING_REMOTE    0b000000100
#define UCP_STREAM_ENDED            0b000001000
#define UCP_STREAM_ENDED_REMOTE     0b000010000
#define UCP_STREAM_DESTROYING       0b000100000
#define UCP_STREAM_DESTROYED        0b001000000
#define UCP_STREAM_DESTROYED_REMOTE 0b010000000
#define UCP_STREAM_CLOSED           0b100000000

#define UCP_PACKET_WAITING  1
#define UCP_PACKET_SENDING  2
#define UCP_PACKET_INFLIGHT 3

#define UCP_PACKET_STREAM_STATE   0b00001
#define UCP_PACKET_STREAM_WRITE   0b00010
#define UCP_PACKET_STREAM_SEND    0b00100
#define UCP_PACKET_STREAM_DESTROY 0b01000
#define UCP_PACKET_SEND           0b10000

#define UCP_HEADER_DATA    0b00001
#define UCP_HEADER_END     0b00010
#define UCP_HEADER_SACK    0b00100
#define UCP_HEADER_MESSAGE 0b01000
#define UCP_HEADER_DESTROY 0b10000

#define UCP_ERROR_DESTROYED        -1
#define UCP_ERROR_DESTROYED_REMOTE -2
#define UCP_ERROR_TIMEOUT          -3

enum UCP_CALLBACK {
  UCP_ON_SEND = 1,
  UCP_ON_MESSAGE = 2,
  UCP_ON_CLOSE = 3,
  UCP_STREAM_ON_DATA = 4,
  UCP_STREAM_ON_END = 5,
  UCP_STREAM_ON_DRAIN = 6,
  UCP_STREAM_ON_ACK = 7,
  UCP_STREAM_ON_CLOSE = 8,
};

// declare these upfront to avoid circular deps.

struct ucp_send;
struct ucp_write;
struct ucp_send;
struct ucp_stream;

typedef struct ucp {
  uv_udp_t handle;
  uv_poll_t io_poll;
  uv_loop_t *loop;
  ucp_fifo_t send_queue;
  uv_timer_t timer;

  int status;
  int readers;
  int events;
  int pending_closes;

  void *userdata;
  int userid;

  struct sockaddr_in on_message_addr;

  void (*on_send)(struct ucp *self, struct ucp_send *req, int failed);
  void (*on_message)(struct ucp *self, const char *buf, size_t buf_len, const struct sockaddr *from);
  void (*on_close)(struct ucp *self);

  uint32_t streams_len;
  uint32_t streams_max_len;
  struct ucp_stream **streams;

  ucp_cirbuf_t streams_by_id;
} ucp_t;

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
  char header[UCP_HEADER_SIZE];
  struct iovec buf[2];
} ucp_packet_t;

typedef struct {
  uint32_t seq; // must be the first entry, so its compat with the cirbuf

  int type;

  struct iovec buf;
} ucp_pending_read_t;

typedef struct {
  uint32_t packets;
  struct ucp_stream *stream;

  void *userdata;
  int userid;
} ucp_write_t;

typedef struct {
  ucp_packet_t pkt;
  struct sockaddr dest;

  void *userdata;
  int userid;
} ucp_send_t;

typedef struct ucp_stream {
  uint32_t local_id; // must be first entry, so its compat with the cirbuf
  uint32_t remote_id;

  int set_id;
  int status;

  ucp_t *ucp;

  struct sockaddr remote_addr;

  void *userdata;
  int userid;

  void (*on_read)(struct ucp_stream *stream, const char *buf, size_t buf_len);
  void (*on_end)(struct ucp_stream *stream);
  void (*on_drain)(struct ucp_stream *stream);
  void (*on_ack)(struct ucp_stream *stream, ucp_write_t *req, int failed, int unordered);
  void (*on_close)(struct ucp_stream *stream, int hard_close);

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

  ucp_cirbuf_t outgoing;
  ucp_cirbuf_t incoming;
} ucp_stream_t;

int
ucp_init (ucp_t *self, uv_loop_t *loop);

int
ucp_set_callback(ucp_t *self, enum UCP_CALLBACK name, void *fn);

int
ucp_send_buffer_size(ucp_t *self, int *value);

int
ucp_recv_buffer_size(ucp_t *self, int *value);

int
ucp_set_ttl(ucp_t *self, int ttl);

int
ucp_bind (ucp_t *self, const struct sockaddr *addr);

int
ucp_getsockname (ucp_t *self, struct sockaddr * name, int *name_len);

int
ucp_send (ucp_t *self, ucp_send_t *req, const char *buf, size_t buf_len, const struct sockaddr *addr);

int
ucp_read_start (ucp_t *self);

int
ucp_read_stop (ucp_t *self);

int
ucp_close (ucp_t *self);

// only exposed here as a convenience / debug tool - the ucp instance tools this automatically
int
ucp_check_timeouts (ucp_t *self);

int
ucp_stream_init (ucp_t *self, ucp_stream_t *stream, uint32_t *local_id);

int
ucp_stream_set_callback(ucp_stream_t *stream, enum UCP_CALLBACK name, void *fn);

void
ucp_stream_connect (ucp_stream_t *stream, uint32_t remote_id, const struct sockaddr *remote_addr);

// only exposed here as a convenience / debug tool - the ucp instance tools this automatically
int
ucp_stream_check_timeouts (ucp_stream_t *stream);

int
ucp_stream_write (ucp_stream_t *stream, ucp_write_t *req, const char *buf, size_t buf_len);

int
ucp_stream_end (ucp_stream_t *stream, ucp_write_t *req);

int
ucp_stream_destroy (ucp_stream_t *stream);

#endif

#ifndef UDX_H
#define UDX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <uv.h>

// TODO: research the packets sizes a bit more
#define UDX_MTU           1400
#define UDX_HEADER_SIZE   20
#define UDX_MAX_DATA_SIZE (UDX_MTU - UDX_HEADER_SIZE)

#define UDX_CLOCK_GRANULARITY_MS 20

#define UDX_MAGIC_BYTE 255
#define UDX_VERSION    1

#define UDX_SOCKET_RECEIVING       0b0001
#define UDX_SOCKET_BOUND           0b0010
#define UDX_SOCKET_CLOSING         0b0100
#define UDX_SOCKET_CLOSING_HANDLES 0b1000

#define UDX_STREAM_CONNECTED        0b00000000001
#define UDX_STREAM_RECEIVING        0b00000000010
#define UDX_STREAM_READING          0b00000000100
#define UDX_STREAM_ENDING           0b00000001000
#define UDX_STREAM_ENDING_REMOTE    0b00000010000
#define UDX_STREAM_ENDED            0b00000100000
#define UDX_STREAM_ENDED_REMOTE     0b00001000000
#define UDX_STREAM_DESTROYING       0b00010000000
#define UDX_STREAM_DESTROYED        0b00100000000
#define UDX_STREAM_DESTROYED_REMOTE 0b01000000000
#define UDX_STREAM_CLOSED           0b10000000000

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

typedef struct {
  uint32_t seq;
} udx_cirbuf_val_t;

typedef struct {
  uint32_t size;
  uint32_t mask;
  udx_cirbuf_val_t **values;
} udx_cirbuf_t;

typedef struct {
  uint32_t btm;
  uint32_t len;
  uint32_t max_len;
  uint32_t mask;
  void **values;
} udx_fifo_t;

typedef struct udx udx_t;
typedef struct udx_stream udx_stream_t;
typedef struct udx_packet udx_packet_t;

typedef struct udx_send udx_send_t;

typedef struct udx_stream_write udx_stream_write_t;
typedef struct udx_stream_send udx_stream_send_t;

typedef void (*udx_preconnect_cb)(udx_t *socket, uint32_t id, struct sockaddr *addr);
typedef void (*udx_send_cb)(udx_send_t *req, int status);
typedef void (*udx_recv_cb)(udx_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from);
typedef void (*udx_close_cb)(udx_t *handle);

typedef void (*udx_stream_read_cb)(udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf);
typedef void (*udx_stream_drain_cb)(udx_stream_t *handle);
typedef void (*udx_stream_ack_cb)(udx_stream_write_t *req, int status, int unordered);
typedef void (*udx_stream_send_cb)(udx_stream_send_t *req, int status);
typedef void (*udx_stream_recv_cb)(udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf);
typedef void (*udx_stream_close_cb)(udx_stream_t *handle, int status);

struct udx {
  uv_udp_t socket;
  uv_poll_t io_poll;
  uv_loop_t *loop;
  udx_fifo_t send_queue;
  uv_timer_t timer;

  int status;
  int readers;
  int events;
  int ttl;
  int pending_closes;

  void *data;

  udx_preconnect_cb on_preconnect;
  udx_recv_cb on_recv;
  udx_close_cb on_close;

  uint32_t streams_len;
  uint32_t streams_max_len;
  udx_stream_t **streams;

  udx_cirbuf_t streams_by_id;
};

struct udx_stream {
  uint32_t local_id; // must be first entry, so its compat with the cirbuf
  uint32_t remote_id;
  uv_loop_t *loop;

  int set_id;
  int status;

  udx_t *socket;

  struct sockaddr remote_addr;

  void *data;

  udx_stream_read_cb on_read;
  udx_stream_recv_cb on_recv;
  udx_stream_drain_cb on_drain;
  udx_stream_close_cb on_close;

  uint32_t seq;
  uint32_t ack;
  uint32_t remote_acked;
  uint32_t remote_ended;

  uint32_t srtt;
  uint32_t rttvar;
  uint32_t rto;

  uint64_t rto_timeout;

  uint32_t pkts_waiting;        // how many packets are added locally but not sent?
  uint32_t pkts_inflight;       // packets inflight to the other peer
  uint32_t pkts_buffered;       // how many (data) packets received but not processed (out of order)?
  uint32_t dup_acks;            // how many duplicate acks received? Used for fast retransmit
  uint32_t retransmits_waiting; // how many retransmits are waiting to be sent? if 0, then inflight iteration is faster

  size_t inflight;
  size_t ssthresh;
  size_t cwnd;
  size_t rwnd;

  size_t stats_sacks;
  size_t stats_pkts_sent;
  size_t stats_fast_rt;

  udx_cirbuf_t outgoing;
  udx_cirbuf_t incoming;
};

struct udx_packet {
  uint32_t seq; // must be the first entry, so its compat with the cirbuf

  int status;
  int type;
  int ttl;

  uint32_t fifo_gc;

  uint8_t transmits;
  uint16_t size;
  uint64_t time_sent;

  void *ctx;

  struct sockaddr dest;

  // just alloc it in place here, easier to manage
  char header[UDX_HEADER_SIZE];
  unsigned int bufs_len;
  uv_buf_t bufs[2];
};

struct udx_send {
  udx_packet_t pkt;
  udx_t *handle;

  udx_send_cb on_send;

  void *data;
};

struct udx_stream_write {
  uint32_t packets;
  udx_stream_t *handle;

  udx_stream_ack_cb on_ack;

  void *data;
};

struct udx_stream_send {
  udx_packet_t pkt;
  udx_stream_t *handle;

  udx_stream_send_cb on_send;

  void *data;
};

int
udx_init (uv_loop_t *loop, udx_t *handle);

int
udx_send_buffer_size (udx_t *handle, int *value);

int
udx_recv_buffer_size (udx_t *handle, int *value);

int
udx_set_ttl (udx_t *handle, int ttl);

int
udx_bind (udx_t *handle, const struct sockaddr *addr);

int
udx_preconnect (udx_t *handle, udx_preconnect_cb cb);

int
udx_getsockname (udx_t *handle, struct sockaddr *name, int *name_len);

int
udx_send (udx_send_t *req, udx_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, const struct sockaddr *addr, udx_send_cb cb);

int
udx_send_ttl (udx_send_t *req, udx_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, const struct sockaddr *addr, int ttl, udx_send_cb cb);

int
udx_recv_start (udx_t *handle, udx_recv_cb cb);

int
udx_recv_stop (udx_t *handle);

int
udx_close (udx_t *handle, udx_close_cb cb);

// only exposed here as a convenience / debug tool - the udx instance uses this automatically
int
udx_check_timeouts (udx_t *handle);

int
udx_stream_init (uv_loop_t *loop, udx_stream_t *handle, uint32_t local_id);

int
udx_stream_connect (udx_stream_t *handle, udx_t *socket, uint32_t remote_id, const struct sockaddr *remote_addr, udx_stream_close_cb close_cb);

int
udx_stream_recv_start (udx_stream_t *handle, udx_stream_recv_cb cb);

int
udx_stream_recv_stop (udx_stream_t *handle);

int
udx_stream_read_start (udx_stream_t *handle, udx_stream_read_cb cb);

int
udx_stream_read_stop (udx_stream_t *handle);

// only exposed here as a convenience / debug tool - the udx instance uses this automatically
int
udx_stream_check_timeouts (udx_stream_t *handle);

int
udx_stream_send (udx_stream_send_t *req, udx_stream_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_send_cb cb);

int
udx_stream_write_resume (udx_stream_t *handle, udx_stream_drain_cb drain_cb);

int
udx_stream_write (udx_stream_write_t *req, udx_stream_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_ack_cb ack_cb);

int
udx_stream_write_end (udx_stream_write_t *req, udx_stream_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, udx_stream_ack_cb ack_cb);

int
udx_stream_destroy (udx_stream_t *handle);

#ifdef __cplusplus
}
#endif
#endif // UDX_H

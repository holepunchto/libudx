#ifndef UDX_H
#define UDX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <uv.h>

// TODO: research the packets sizes a bit more
#define UDX_DEFAULT_MTU 1200
#define UDX_HEADER_SIZE 20

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
typedef struct udx_socket udx_socket_t;
typedef struct udx_stream udx_stream_t;
typedef struct udx_packet udx_packet_t;

typedef struct udx_socket_send udx_socket_send_t;

typedef struct udx_stream_write udx_stream_write_t;
typedef struct udx_stream_send udx_stream_send_t;

typedef enum {
  UDX_LOOKUP_FAMILY_IPV4 = 1,
  UDX_LOOKUP_FAMILY_IPV6 = 2,
} udx_lookup_flags;

typedef struct udx_lookup udx_lookup_t;

typedef struct udx_interface_event udx_interface_event_t;

typedef void (*udx_socket_send_cb)(udx_socket_send_t *req, int status);
typedef void (*udx_socket_recv_cb)(udx_socket_t *handle, ssize_t read_len, const uv_buf_t *buf, const struct sockaddr *from);
typedef void (*udx_socket_close_cb)(udx_socket_t *handle);

typedef int (*udx_stream_firewall_cb)(udx_stream_t *handle, udx_socket_t *socket, const struct sockaddr *from);
typedef void (*udx_stream_read_cb)(udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf);
typedef void (*udx_stream_drain_cb)(udx_stream_t *handle);
typedef void (*udx_stream_ack_cb)(udx_stream_write_t *req, int status, int unordered);
typedef void (*udx_stream_send_cb)(udx_stream_send_t *req, int status);
typedef void (*udx_stream_recv_cb)(udx_stream_t *handle, ssize_t read_len, const uv_buf_t *buf);
typedef void (*udx_stream_close_cb)(udx_stream_t *handle, int status);

typedef void (*udx_lookup_cb)(udx_lookup_t *handle, int status, const struct sockaddr *addr, int addr_len);

typedef void (*udx_interface_event_cb)(udx_interface_event_t *handle, int status);
typedef void (*udx_interface_event_close_cb)(udx_interface_event_t *handle);

struct udx {
  uv_timer_t timer;
  uv_loop_t *loop;

  uint32_t refs;
  uint32_t sockets;
  udx_socket_t *timer_closed_by;

  uint32_t streams_len;
  uint32_t streams_max_len;
  udx_stream_t **streams;

  udx_cirbuf_t streams_by_id;
};

struct udx_socket {
  uv_udp_t socket;
  uv_poll_t io_poll;
  udx_fifo_t send_queue;

  udx_t *udx;
  udx_cirbuf_t *streams_by_id; // for convenience

  int status;
  int readers;
  int events;
  int ttl;
  int pending_closes;

  void *data;

  udx_socket_recv_cb on_recv;
  udx_socket_close_cb on_close;
};

struct udx_stream {
  uint32_t local_id; // must be first entry, so its compat with the cirbuf
  uint32_t remote_id;

  int set_id;
  int status;
  int out_of_order;
  int recovery;

  udx_t *udx;
  udx_socket_t *socket;

  struct sockaddr_storage remote_addr;
  int remote_addr_len;

  void *data;

  udx_stream_firewall_cb on_firewall;
  udx_stream_read_cb on_read;
  udx_stream_recv_cb on_recv;
  udx_stream_drain_cb on_drain;
  udx_stream_close_cb on_close;

  uint16_t mtu;

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

  udx_fifo_t unordered;
};

struct udx_packet {
  uint32_t seq; // must be the first entry, so its compat with the cirbuf

  int status;
  int type;
  int ttl;
  int is_retransmit;

  uint32_t fifo_gc;

  uint8_t transmits;
  uint16_t size;
  uint64_t time_sent;

  void *ctx;

  struct sockaddr_storage dest;
  int dest_len;

  // just alloc it in place here, easier to manage
  char header[UDX_HEADER_SIZE];
  unsigned int bufs_len;
  uv_buf_t bufs[2];
};

struct udx_socket_send {
  udx_packet_t pkt;
  udx_socket_t *handle;

  udx_socket_send_cb on_send;

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

struct udx_lookup {
  uv_getaddrinfo_t req;
  struct addrinfo hints;

  udx_lookup_cb on_lookup;

  void *data;
};

struct udx_interface_event {
  uv_timer_t timer;
  uv_loop_t *loop;

  uv_interface_address_t *addrs;
  int addrs_len;
  bool sorted;

  udx_interface_event_cb on_event;
  udx_interface_event_close_cb on_close;

  void *data;
};

int
udx_init (uv_loop_t *loop, udx_t *handle);

int
udx_socket_init (udx_t *handle, udx_socket_t *socket);

int
udx_socket_get_send_buffer_size (udx_socket_t *handle, int *value);

int
udx_socket_set_send_buffer_size (udx_socket_t *handle, int value);

int
udx_socket_get_recv_buffer_size (udx_socket_t *handle, int *value);

int
udx_socket_set_recv_buffer_size (udx_socket_t *handle, int value);

int
udx_socket_get_ttl (udx_socket_t *handle, int *ttl);

int
udx_socket_set_ttl (udx_socket_t *handle, int ttl);

int
udx_socket_bind (udx_socket_t *handle, const struct sockaddr *addr);

int
udx_socket_getsockname (udx_socket_t *handle, struct sockaddr *name, int *name_len);

int
udx_socket_send (udx_socket_send_t *req, udx_socket_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, const struct sockaddr *addr, udx_socket_send_cb cb);

int
udx_socket_send_ttl (udx_socket_send_t *req, udx_socket_t *handle, const uv_buf_t bufs[], unsigned int bufs_len, const struct sockaddr *addr, int ttl, udx_socket_send_cb cb);

int
udx_socket_recv_start (udx_socket_t *handle, udx_socket_recv_cb cb);

int
udx_socket_recv_stop (udx_socket_t *handle);

int
udx_socket_close (udx_socket_t *handle, udx_socket_close_cb cb);

// only exposed here as a convenience / debug tool - the udx instance uses this automatically
int
udx_check_timeouts (udx_t *handle);

int
udx_stream_init (udx_t *udx, udx_stream_t *handle, uint32_t local_id, udx_stream_close_cb close_cb);

int
udx_stream_get_mtu (udx_stream_t *handle, uint16_t *mtu);

int
udx_stream_set_mtu (udx_stream_t *handle, uint16_t mtu);

int
udx_stream_get_seq (udx_stream_t *handle, uint32_t *seq);

int
udx_stream_set_seq (udx_stream_t *handle, uint32_t seq);

int
udx_stream_get_ack (udx_stream_t *handle, uint32_t *ack);

int
udx_stream_set_ack (udx_stream_t *handle, uint32_t ack);

int
udx_stream_connect (udx_stream_t *handle, udx_socket_t *socket, uint32_t remote_id, const struct sockaddr *remote_addr);

int
udx_stream_firewall (udx_stream_t *handle, udx_stream_firewall_cb firewall_cb);

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

int
udx_lookup (uv_loop_t *loop, udx_lookup_t *req, const char *host, unsigned int flags, udx_lookup_cb cb);

int
udx_interface_event_init (uv_loop_t *loop, udx_interface_event_t *handle);

int
udx_interface_event_start (udx_interface_event_t *handle, udx_interface_event_cb cb, uint64_t frequency);

int
udx_interface_event_stop (udx_interface_event_t *handle);

int
udx_interface_event_close (udx_interface_event_t *handle, udx_interface_event_close_cb cb);

#ifdef __cplusplus
}
#endif
#endif // UDX_H

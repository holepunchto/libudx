#ifndef UDX_INTERNAL_H
#define UDX_INTERNAL_H

#include "../include/udx.h"

#define UDX_PACKET_CALLBACK     (UDX_PACKET_TYPE_STREAM_SEND | UDX_PACKET_TYPE_STREAM_DESTROY | UDX_PACKET_TYPE_SOCKET_SEND)
#define UDX_PACKET_FREE_ON_SEND (UDX_PACKET_TYPE_STREAM_STATE | UDX_PACKET_TYPE_STREAM_DESTROY | UDX_PACKET_TYPE_STREAM_RELAY)

#define UDX_UNUSED(x) ((void) (x))

typedef struct {
  uint64_t prior_timestamp;
  uint32_t prior_delivered;

  int32_t delivered;
  int64_t interval_ms;
  uint32_t snd_interval_ms;
  uint32_t rcv_interval_ms;
  int64_t rtt_ms; // rs.rtt in IETF draft
  int losses;
  uint32_t acked_sacked;
  uint32_t prior_in_flight;
  uint32_t seq;
  bool is_app_limited;
  bool is_retrans;
  bool is_ack_delayed;
} udx_rate_sample_t;

static inline uint32_t
max_uint32 (uint32_t a, uint32_t b) {
  return a < b ? b : a;
}

static inline uint32_t
min_uint32 (uint32_t a, uint32_t b) {
  return a < b ? a : b;
}

static inline int32_t
max_int32 (int32_t a, int32_t b) {
  return a < b ? b : a;
}

static inline int64_t
max_int64 (int64_t a, int64_t b) {
  return a < b ? b : a;
}

static inline uint64_t
min_uint64 (uint64_t a, uint64_t b) {
  return a < b ? a : b;
}

static inline uint64_t
max_uint64 (uint64_t a, uint64_t b) {
  return a < b ? b : a;
}

static inline int32_t
seq_diff (uint32_t a, uint32_t b) {
  return a - b;
}

static inline int
seq_compare (uint32_t a, uint32_t b) {
  int32_t d = seq_diff(a, b);
  return d < 0 ? -1 : d > 0 ? 1
                            : 0;
}

static inline bool
rack_sent_after (uint64_t t1, uint32_t seq1, uint64_t t2, uint32_t seq2) {
  return t1 > t2 || (t1 == t2 && seq_compare(seq2, seq1) < 0);
}

uint32_t
udx__max_payload (udx_stream_t *stream);

void
udx__close_handles (udx_socket_t *socket);

void
udx__rate_pkt_sent (udx_stream_t *stream, udx_packet_t *pkt);

void
udx__rate_pkt_delivered (udx_stream_t *stream, udx_packet_t *pkt, udx_rate_sample_t *rs);

void
udx__rate_gen (udx_stream_t *stream, uint32_t delivered, uint32_t lost, udx_rate_sample_t *rs);

void
udx__rate_check_app_limited (udx_stream_t *stream);

// bbr

void
bbr_init (udx_stream_t *stream);

void
bbr_on_loss (udx_stream_t *stream);

void
bbr_main (udx_stream_t *stream, udx_rate_sample_t *rs);

void
bbr_on_transmit_start (udx_stream_t *stream, uint64_t now_ms);

#endif // UDX_INTERNAL_H

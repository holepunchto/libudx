
#include "debug.h"
#include "internal.h"

static uint32_t
max_uint32 (uint32_t a, uint32_t b) {
  return a < b ? b : a;
}

// snapshot current delivery information in the packet,
// to generate a rate sample later when packet is acked
void
udx__rate_pkt_sent (udx_stream_t *stream, udx_packet_t *pkt) {
  if (stream->packets_tx == 1) {
    // first packet was just sent
    uint64_t now_ms = uv_now(stream->udx->loop);

    stream->first_time_sent = now_ms;
    stream->delivered_time = now_ms;
  }

  pkt->first_time_sent = stream->first_time_sent;
}

void
udx__rate_pkt_delivered (udx_stream_t *stream, udx_packet_t *pkt, rate_sample_t *rs) {

  if (!pkt->delivered_time) {
    return;
  }

  if (!rs->prior_delivered || rack_sent_after(pkt->time_sent, stream->first_time_sent, pkt->seq, rs->seq)) {
    rs->prior_delivered = pkt->delivered;
    rs->prior_timestamp = pkt->delivered_time;
    rs->is_app_limited = pkt->is_app_limited;
    rs->is_retrans = pkt->retransmitted;
    rs->seq = pkt->seq;

    stream->first_time_sent = pkt->time_sent;

    rs->interval_ms = stream->first_time_sent - pkt->first_time_sent;
  }

  // if (pkt->sacked) {
  //   pkt->delivered_time = 0;
  // }
}

static inline uint32_t
stamp_ms_delta (uint64_t t1, uint64_t t0) {
  return (int64_t) t1 - (int64_t) t0;
}

// generate a rate sample and store it in the udx_stream_t
void
udx__rate_gen (udx_stream_t *stream, uint32_t delivered, uint32_t lost, bool is_sack_reneg, rate_sample_t *rs) {
  uint64_t now_ms = uv_now(stream->udx->loop);

  // clear app limited if bubble is acked and gone
  if (stream->app_limited && seq_compare(stream->delivered, stream->app_limited) > 0) {
    stream->app_limited = 0;
  }

  if (delivered) {
    stream->delivered_time = now_ms;
  }

  rs->acked_sacked = delivered;
  rs->losses = lost;

  if (!rs->prior_timestamp || is_sack_reneg) {
    rs->delivered = -1;
    rs->interval_ms = -1;
    return;
  }

  rs->delivered = stream->delivered - rs->prior_delivered;

  uint32_t snd_ms = rs->interval_ms;
  uint32_t ack_ms = stamp_ms_delta(now_ms, rs->prior_timestamp);

  rs->interval_ms = max_uint32(snd_ms, ack_ms);

  rs->snd_interval_ms = snd_ms;
  rs->rcv_interval_ms = ack_ms;

  char *ca_state_string[] = {
    "Unknown",
    "UDX_CA_OPEN",
    "UDX_CA_RECOVERY",
    "UDX_CA_LOSS",
  };

  if (rs->interval_ms < stream->rack_rtt_min) {
    if (!rs->is_retrans) {
      debug_printf("rs->interval_ms=%ld, rs->delivered=%d, stream->ca_state=%s, stream->min_rtt=%u", rs->interval_ms, rs->delivered, ca_state_string[stream->ca_state], stream->rack_rtt_min);
    }
    rs->interval_ms = -1;
    return;
  }

  if (!rs->is_app_limited || ((uint64_t) rs->delivered * stream->rate_interval_ms >= (uint64_t) stream->rate_delivered * rs->interval_ms)) {
    stream->rate_delivered = rs->delivered;
    stream->rate_interval_ms = rs->interval_ms;
    stream->rate_sample_is_app_limited = rs->is_app_limited;
  }
}

void
udx__rate_check_app_limited (udx_stream_t *stream) {
  if (stream->writes_queued_bytes < udx__max_payload(stream) &&
      stream->inflight_queue.len < stream->cwnd &&
      stream->retransmit_queue.len == 0) {
    stream->app_limited = stream->delivered + stream->inflight_queue.len ?: 1;
  }
}

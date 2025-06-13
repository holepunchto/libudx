
#include "debug.h"
#include "internal.h"

// store current delivery information in the packet
// to generate a sample when it is acked
void
udx__rate_pkt_sent (udx_stream_t *stream, udx_packet_t *pkt) {
  if (!stream->packets_tx) {
    // first packet was just sent
    uint64_t now_ms = uv_now(stream->udx->loop);

    stream->interval_start_time = now_ms;
    stream->delivered_time = now_ms;
  }

  pkt->interval_start_time = stream->interval_start_time;

  pkt->delivered_time = stream->delivered_time;
  pkt->delivered = stream->delivered;
  pkt->is_app_limited = stream->app_limited ? true : false;
}

static inline uint32_t
stamp_ms_delta (uint64_t t1, uint64_t t0) {
  int64_t delta = (int64_t) t1 - (int64_t) t0;
  return max_int64(delta, 0L);
}

void
udx__rate_pkt_delivered (udx_stream_t *stream, udx_packet_t *pkt, udx_rate_sample_t *rs) {

  if (!pkt->delivered_time) {
    return;
  }

  if (!rs->prior_delivered || rack_sent_after(pkt->time_sent, pkt->seq, stream->interval_start_time, rs->seq)) {
    rs->prior_delivered = pkt->delivered;

    rs->prior_timestamp = pkt->delivered_time;
    rs->is_app_limited = pkt->is_app_limited;
    rs->is_retrans = pkt->retransmitted;
    rs->seq = pkt->seq;

    // record send time of most recently ACKed packet
    stream->interval_start_time = pkt->time_sent;

    rs->interval_ms = stamp_ms_delta(stream->interval_start_time, pkt->interval_start_time);
  }
}

// generate a rate sample and store it in the udx_stream_t
void
udx__rate_gen (udx_stream_t *stream, uint32_t delivered, uint32_t lost, udx_rate_sample_t *rs) {
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

  if (!rs->prior_timestamp) {
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

  if (rs->interval_ms < udx_rtt_min(stream)) {
    if (!rs->is_retrans) {
      debug_printf("rs->interval_ms=%ld, rs->delivered=%d, stream->ca_state=%s, stream->min_rtt=%u\n", rs->interval_ms, rs->delivered, ca_state_string[stream->ca_state], udx_rtt_min(stream));
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

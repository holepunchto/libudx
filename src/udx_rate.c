
#include "debug.h"
#include "internal.h"

// store current delivery information in the packet
// to generate a rate sample when it is acked
void
udx__rate_pkt_sent (udx_stream_t *stream, udx_packet_t *pkt) {
  if (stream->seq == stream->remote_acked) {
    // there is no data in flight, so start the rate
    // sample intervals of this first flight with the current time.
    uint64_t now_ms = uv_now(stream->udx->loop);

    stream->first_sent_ts = now_ms;
    stream->delivered_ts = now_ms;
  }

  pkt->first_sent_ts = stream->first_sent_ts;

  pkt->delivered_ts = stream->delivered_ts;
  pkt->delivered = stream->delivered;
  pkt->is_app_limited = stream->app_limited ? true : false;

  debug_throughput(stream);
}

static inline uint32_t
stamp_ms_delta (uint64_t t1, uint64_t t0) {
  int64_t delta = (int64_t) t1 - (int64_t) t0;
  return max_int64(delta, 0L);
}

void
udx__rate_pkt_delivered (udx_stream_t *stream, udx_packet_t *pkt, udx_rate_sample_t *rs) {

  if (!pkt->delivered_ts) {
    return;
  }

  if (!rs->prior_delivered || rack_sent_after(pkt->time_sent, pkt->seq, stream->first_sent_ts, rs->seq)) {
    rs->prior_delivered = pkt->delivered;

    rs->prior_timestamp = pkt->delivered_ts;
    rs->is_app_limited = pkt->is_app_limited;
    rs->is_retrans = pkt->retransmitted;
    rs->seq = pkt->seq;

    // record send time of most recently ACKed packet
    stream->first_sent_ts = pkt->time_sent;

    rs->interval_ms = stamp_ms_delta(stream->first_sent_ts, pkt->first_sent_ts);
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
    stream->delivered_ts = now_ms;
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

  debug_throughput(stream);
}

void
udx__rate_check_app_limited (udx_stream_t *stream) {
  if (stream->writes_queued_bytes < udx__max_payload(stream) &&
      stream->inflight_queue.len < stream->cwnd &&
      stream->retransmit_queue.len == 0) {
    stream->app_limited = stream->delivered + stream->inflight_queue.len ?: 1; // can't use sequence zero since it indicates no app limit.
  }
}

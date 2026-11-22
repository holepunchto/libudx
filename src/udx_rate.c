
#include "debug.h"
#include "internal.h"

// store current delivery information in the packet
// to generate a rate sample when it is acked
void
udx__rate_pkt_sent (udx_stream_t *stream, udx_packet_t *pkt, uint64_t now_ns) {
  if (stream->seq == stream->remote_acked) {
    // there is no data in flight, so start the rate
    // sample intervals of this first flight with the current time.
    stream->first_sent_ts = now_ns;
    stream->delivered_ts = now_ns;
  }

  pkt->first_sent_ts = stream->first_sent_ts;

  pkt->delivered_ts = stream->delivered_ts;
  pkt->delivered = stream->delivered;
  pkt->is_app_limited = stream->app_limited ? true : false;

  debug_throughput(stream);
}

static inline uint64_t
stamp_ns_delta (uint64_t t1, uint64_t t0) {
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

    rs->interval_ns = stamp_ns_delta(stream->first_sent_ts, pkt->first_sent_ts);
  }
}

// generate a rate sample and store it in the udx_stream_t
void
udx__rate_gen (udx_stream_t *stream, uint32_t delivered, uint32_t lost, udx_rate_sample_t *rs) {
  uint64_t now_ns = uv_hrtime();

  // clear app limited if bubble is acked and gone
  if (stream->app_limited && seq_compare(stream->delivered, stream->app_limited) > 0) {
    stream->app_limited = 0;
  }

  if (delivered) {
    stream->delivered_ts = now_ns;
  }

  rs->acked_sacked = delivered;
  rs->losses = lost;

  if (!rs->prior_timestamp) {
    rs->delivered = -1;
    rs->interval_ns = -1;
    return;
  }

  rs->delivered = stream->delivered - rs->prior_delivered;

  uint64_t snd_ns = rs->interval_ns;
  uint64_t ack_ns = stamp_ns_delta(now_ns, rs->prior_timestamp);

  rs->interval_ns = max_uint64(snd_ns, ack_ns);

  rs->snd_interval_ns = snd_ns;
  rs->rcv_interval_ns = ack_ns;

  char *ca_state_string[] = {
    "Unknown",
    "UDX_CA_OPEN",
    "UDX_CA_RECOVERY",
    "UDX_CA_LOSS",
  };

  if ((uint64_t) rs->interval_ns < udx_rtt_min(stream)) {
    if (!rs->is_retrans) {
      debug_printf("rs->interval_ns=%" PRId64 ", rs->delivered=%d, stream->ca_state=%s, stream->min_rtt_ns=%" PRIu64 "\n", rs->interval_ns, rs->delivered, ca_state_string[stream->ca_state], udx_rtt_min(stream));
    }
    rs->interval_ns = -1;
    return;
  }

  if (!rs->is_app_limited || ((uint64_t) rs->delivered * stream->rate_interval_ns >= (uint64_t) stream->rate_delivered * (uint64_t) rs->interval_ns)) {
    stream->rate_delivered = rs->delivered;
    stream->rate_interval_ns = rs->interval_ns;
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

#include "../include/udx.h"
#include "debug.h"
#include "internal.h"
#include "win_filter.h"
#include "win_filter_f64.h"
#include <assert.h>

// the BBR module takes an input rate sample (udx_rate_sample_t)
// that is generated once per ACK. given the rate sample we update
// two variables:
//     bw = windowed_max(delivered, 10 * rtt)
//     min_rtt = windowed_min(rtt, 10 seconds)
// and we set two output variables:
//     pacing_rate = pacing_gain * bw
//     cwnd = max(cwnd_gain * bw * min_rtt, 4)

// some simplifications vs the Linux kernel's BBR implemntation
// 1. we use double for gain, etc, since we are not limited to integer registers
// 2. bw and bdp are measured in packets, e.g. bdp of 20 means that the network can
// support 20 packets in flight. the idea with bbr is to set the cwnd to our estimate
// of the BDP
// TODO: confirm we've corrected this everywhere, as we mixes packets/second for some measurements in the past
// 3. we use milliseconds for time measurements

#define UDX_BBR_STATE_STARTUP   0
#define UDX_BBR_STATE_DRAIN     1
#define UDX_BBR_STATE_PROBE_BW  2
#define UDX_BBR_STATE_PROBE_RTT 3

#define UDX_BBR_CYCLE_LEN             8
#define UDX_BBR_BW_FILTER_CYCLES      (UDX_BBR_CYCLE_LEN + 2)
#define UDX_BBR_MIN_RTT_INTERVAL_MS   10000
#define UDX_BBR_MIN_PROBE_RTT_MODE_MS 200

static const double bbr_pacing_margin_percent = 0.99;
static const double bbr_high_gain = 2.88539; // 2/ln(2)
static const double bbr_drain_gain = 1 / 2.88539;
static const double bbr_cwnd_gain = 2.0;

static double bbr_pacing_gain[] = {
  1.25,
  0.75,
  1.0,
  1.0,
  1.0,
  1.0,
  1.0,
  1.0
};

static const uint32_t bbr_cwnd_min_target = 4;

// bw increased to full bw probe, there may be more bw available
static const double bbr_full_bw_thresh = 1.25;
// if after 3 rounds w/o growth, estimate that the pipe is full
// todo: waiting three rounds is in part to work around rwin auto tuning. Can we remove this?
static const uint32_t bbr_full_bw_count = 3;

static const double bbr_extra_acked_gain = 1.0;
static const uint32_t bbr_extra_acked_win_rtts = 5;
static const uint32_t bbr_ack_epoch_acked_reset_thresh = UINT32_MAX;

static const uint32_t bbr_extra_acked_max_ms = 100;

// windowed max recent bandwidth sample, packets/ms
static double
bbr_max_bw (udx_stream_t *stream) {
  return win_filter_f64_get(&stream->bbr.bw);
}

static bool
bbr_full_bw_reached (udx_stream_t *stream) {
  return stream->bbr.full_bw_reached;
}

// return the estimated bandwidth for the stream, pkts/ms
static double
bbr_bw (udx_stream_t *stream) {
  return bbr_max_bw(stream);
}

static uint16_t
bbr_extra_acked (udx_stream_t *stream) {
  return max_uint32(stream->bbr.extra_acked[0], stream->bbr.extra_acked[1]);
}

static uint64_t
bbr_rate_in_bytes (udx_stream_t *stream, double bw, double gain) {
  uint32_t mss = udx__max_payload(stream);

  return bw * mss * gain * bbr_pacing_margin_percent;
}

static uint64_t
bbr_bw_to_pacing_rate (udx_stream_t *stream, double bw, double gain) {
  return bbr_rate_in_bytes(stream, bw, gain);
}

static void
bbr_init_pacing_rate_from_rtt (udx_stream_t *stream) {
  uint32_t srtt = 0;
  if (stream->srtt) {
    srtt = stream->srtt;
    stream->bbr.has_seen_rtt = 1;
  } else {
    srtt = 1;
  }

  uint64_t rate = stream->cwnd;
  double bw = (double) rate / srtt;

  assert(bw != 0);

  stream->pacing_bytes_per_ms = bbr_bw_to_pacing_rate(stream, bw, bbr_high_gain);
}

static void
bbr_set_pacing_rate (udx_stream_t *stream, double bw, double gain) {
  uint64_t rate_bpms = bbr_bw_to_pacing_rate(stream, bw, gain);

  if (!stream->bbr.has_seen_rtt && stream->srtt) {
    bbr_init_pacing_rate_from_rtt(stream);
  }

  if (stream->bbr.full_bw_reached || rate_bpms > stream->pacing_bytes_per_ms) {
    stream->pacing_bytes_per_ms = rate_bpms;
  }
}

static void
bbr_save_cwnd (udx_stream_t *stream) {
  // save last known cwnd when entering recovery, we use it to restore
  // after completing recovery successfully
  if (stream->bbr.prev_ca_state < UDX_CA_RECOVERY && stream->bbr.state != UDX_BBR_MIN_PROBE_RTT_MODE_MS) {
    stream->bbr.prior_cwnd = stream->cwnd;
  } else {
    stream->bbr.prior_cwnd = max_uint32(stream->bbr.prior_cwnd, stream->cwnd);
  }
}

static void
bbr_check_probe_rtt_done (udx_stream_t *stream, uint64_t now_ms);

void
bbr_on_transmit_start (udx_stream_t *stream, uint64_t now_ms) {
  // BBR 4.2.2
  if (stream->app_limited) {
    stream->bbr.idle_restart = true;
    stream->bbr.ack_epoch_start = now_ms;
    stream->bbr.ack_epoch_acked = 0;

    if (stream->bbr.state == UDX_BBR_STATE_PROBE_BW) {
      bbr_set_pacing_rate(stream, bbr_bw(stream), 1.0);
    } else if (stream->bbr.state == UDX_BBR_STATE_PROBE_RTT) {
      bbr_check_probe_rtt_done(stream, now_ms);
    }
  }
}

// calculate bdp based on min RTT and the estimated bottleneck bandwidth
// bdp = bw * min_rtt * gain, unit is bytes
// note: bw is in packets!

static uint32_t
bbr_bdp (udx_stream_t *stream, double bw, double gain) {

  if (stream->bbr.min_rtt_ms == ~0U) {
    return 10; // cap at initial cwnd
  }

  return bw * stream->bbr.min_rtt_ms * gain;
}

static uint32_t
bbr_inflight (udx_stream_t *stream, double bw, double gain) {
  return bbr_bdp(stream, bw, gain);
}

// Return the highest recently seen degree of ack aggregation. unit is packets.
// aggregation is the amount acked during an rtt interval greater than
// the amount predicted by the current pacing rate. (acked - expected_acked)
// This is used to set a higher cwnd, allowing us to continue sending
// during interruptions of the ack stream.

static uint32_t
bbr_ack_aggregation_cwnd (udx_stream_t *stream) {
  uint32_t aggr_cwnd = 0;
  if (bbr_extra_acked_gain && bbr_full_bw_reached(stream)) {
    // clamps extra aggregation packets to at most extra_acked_max_ms (100ms) of packets at current bw
    uint32_t max_aggr_cwnd = bbr_bw(stream) * bbr_extra_acked_max_ms;

    aggr_cwnd = (bbr_extra_acked_gain * bbr_extra_acked(stream));
    aggr_cwnd = min_uint32(aggr_cwnd, max_aggr_cwnd);
  }

  return aggr_cwnd;
}

static bool
bbr_set_cwnd_to_recover_or_restore (udx_stream_t *stream, udx_rate_sample_t *rs, uint32_t acked, uint32_t *cwnd_out) {

  uint8_t prev_state = stream->bbr.prev_ca_state;
  uint8_t state = stream->ca_state;
  uint32_t cwnd = stream->cwnd;

  // an ack for p pkts should release at most 2*p packets.
  // step 1. deduct the number of lost packets
  // step 2. in bbr_set_cwnd() we slow start up to the target cwnd

  if (rs->losses > 0) {
    cwnd = max_int32(cwnd - rs->losses, 1); // treat as signed
  }

  if (state == UDX_CA_RECOVERY && prev_state != UDX_CA_RECOVERY) {
    stream->bbr.use_packet_conservation = true;
    stream->bbr.next_rtt_delivered = stream->delivered;
    cwnd = stream->inflight_queue.len + acked;
  } else if (prev_state >= UDX_CA_RECOVERY && state == UDX_CA_OPEN) {
    cwnd = max_uint32(cwnd, stream->bbr.prior_cwnd);
    stream->bbr.use_packet_conservation = false;
  }

  stream->bbr.prev_ca_state = state;

  if (stream->bbr.use_packet_conservation) {
    *cwnd_out = max_uint32(cwnd, stream->inflight_queue.len + acked);
    return true;
  }
  *cwnd_out = cwnd;
  return false;
}

static void
bbr_set_cwnd (udx_stream_t *stream, udx_rate_sample_t *rs, uint32_t acked, double bw, double gain) {
  uint32_t cwnd = stream->cwnd;

  if (!acked) goto done;

  if (bbr_set_cwnd_to_recover_or_restore(stream, rs, acked, &cwnd)) {
    goto done;
  }

  uint32_t target_cwnd = bbr_bdp(stream, bw, gain);

  target_cwnd += bbr_ack_aggregation_cwnd(stream);

  if (bbr_full_bw_reached(stream)) {
    cwnd = min_uint32(cwnd + acked, target_cwnd);
  } else if (cwnd < target_cwnd || stream->delivered < 10) {
    cwnd = cwnd + acked;
  }

  cwnd = max_uint32(cwnd, bbr_cwnd_min_target);
done:
  stream->cwnd = cwnd;
  if (stream->bbr.state == UDX_BBR_STATE_PROBE_RTT) {
    stream->cwnd = min_uint32(stream->cwnd, bbr_cwnd_min_target);
  }
}

static uint32_t
time_delta_ms (uint64_t t1, uint64_t t0) {
  return max_uint32((int64_t) t1 - (int64_t) t0, 0);
}

static bool
bbr_is_next_cycle_phase (udx_stream_t *stream, udx_rate_sample_t *rs) {
  bool is_full_length = time_delta_ms(stream->delivered_ts, stream->bbr.cycle_timestamp) > stream->bbr.min_rtt_ms;

  if (stream->bbr.pacing_gain == 1.0) {
    return is_full_length;
  }

  uint32_t inflight = stream->inflight_queue.len;
  double bw = bbr_max_bw(stream);

  if (stream->bbr.pacing_gain > 1.0) {
    return is_full_length && (rs->losses || inflight > bbr_inflight(stream, bw, stream->bbr.pacing_gain));
  }

  return is_full_length || inflight <= bbr_inflight(stream, bw, 1.0);
}

static void
bbr_advance_cycle_phase (udx_stream_t *stream) {
  stream->bbr.cycle_index = (stream->bbr.cycle_index + 1) & (UDX_BBR_CYCLE_LEN - 1);
  // debug_printf("bbr: cycle_index advanced: index=%d time=%ld rate=%1.2f\n", stream->bbr.cycle_index, stream->delivered_ts, bbr_pacing_gain[stream->bbr.cycle_index]);
  stream->bbr.cycle_timestamp = stream->delivered_ts;
}

static void
bbr_update_cycle_phase (udx_stream_t *stream, udx_rate_sample_t *rs) {
  if (stream->bbr.state == UDX_BBR_STATE_PROBE_BW && bbr_is_next_cycle_phase(stream, rs)) {
    bbr_advance_cycle_phase(stream);
  }
}

static void
bbr_reset_startup_mode (udx_stream_t *stream) {
  // 4.3.1.1
  stream->bbr.state = UDX_BBR_STATE_STARTUP;
  // debug_printf("bbr: rid=%u entering STARTUP\n", stream->remote_id);
}

static void
bbr_reset_probe_bw_mode (udx_stream_t *stream) {
  if (stream->bbr.state != UDX_BBR_STATE_PROBE_BW) {
    debug_throughput_printf(stream, "probe_bw");
  }
  stream->bbr.state = UDX_BBR_STATE_PROBE_BW;
  // debug_printf("bbr: rid=%u entering PROBE_BW time=%ld\n", stream->remote_id, uv_now(stream->udx->loop));
  // todo: use an rng?
  // probable reasoning: multiple streams may start simultaneously in many scenarios,
  // starting on random phases makes them more fair by staggering their rttprobe / backoff phases
  // stream->bbr.cycle_index = UDX_BBR_CYCLE_LEN - 1 - get_random_u32_below(bbr_cycle_rand);

  stream->bbr.cycle_index = 3; // should be randomly chosen.
  bbr_advance_cycle_phase(stream);
}

static void
bbr_reset_mode (udx_stream_t *stream) {
  if (!bbr_full_bw_reached(stream)) {
    bbr_reset_startup_mode(stream);
  } else {
    bbr_reset_probe_bw_mode(stream);
  }
}

static void
bbr_update_bw (udx_stream_t *stream, udx_rate_sample_t *rs) {
  stream->bbr.round_start = false;

  if (rs->delivered < 0 || rs->interval_ms <= 0) {
    return;
  }

  // see if we've reached the next RTT
  if (seq_diff(rs->prior_delivered, stream->bbr.next_rtt_delivered) >= 0) {
    stream->bbr.next_rtt_delivered = stream->delivered;
    stream->bbr.rtt_count++;
    stream->bbr.round_start = true;
    stream->bbr.use_packet_conservation = false;
  }

  // divide delivered / interval to find a bw sample (in packets/ms)
  double bw = (double) rs->delivered / rs->interval_ms;
  assert(bw != 0);

  if (!rs->is_app_limited || bw >= bbr_max_bw(stream)) {
    // debug_printf("bw=%lu (rs->delivered=%u / rs->interval_ms=%ld)\n", bw, rs->delivered, rs->interval_ms);
    win_filter_f64_apply_max(&stream->bbr.bw, UDX_BBR_CYCLE_LEN + 2, stream->bbr.rtt_count, bw);
  }
}

// Estimates the windows max degree of ACK aggregation
// unlike Linux TCP we always aggregate ACKs on the sender side in the current implementation
static void
bbr_update_ack_aggregation (udx_stream_t *stream, udx_rate_sample_t *rs) {
  if (!bbr_extra_acked_gain || rs->acked_sacked <= 0 || rs->delivered < 0 || rs->interval_ms <= 0) {
    return;
  }

  if (stream->bbr.round_start) {
    // redundant since we clamp to bbr_extra_acked_win_rtts
    stream->bbr.extra_acked_win_rtts = min_uint32(0xff, stream->bbr.extra_acked_win_rtts + 1);
    if (stream->bbr.extra_acked_win_rtts >= bbr_extra_acked_win_rtts) {
      stream->bbr.extra_acked_win_rtts = 0;
      stream->bbr.extra_acked_win_index = stream->bbr.extra_acked_win_index ? 0 : 1;
      stream->bbr.extra_acked[stream->bbr.extra_acked_win_index] = 0;
    }
  }

  uint32_t epoch_ms = time_delta_ms(stream->delivered_ts, stream->bbr.ack_epoch_start);
  uint32_t expected_acked = bbr_bw(stream) * epoch_ms;

  // reset aggregation epoch if
  // 1. ACK rate is below expected or
  // 2. an (improbably large) # of acks that would overflow ack_epoch_acked arrives
  if (stream->bbr.ack_epoch_acked <= expected_acked || ((uint64_t) stream->bbr.ack_epoch_acked + rs->acked_sacked >= bbr_ack_epoch_acked_reset_thresh)) {
    stream->bbr.ack_epoch_acked = 0;
    stream->bbr.ack_epoch_start = stream->delivered_ts;
    expected_acked = 0;
  }

  assert(stream->bbr.ack_epoch_acked + rs->acked_sacked > stream->bbr.ack_epoch_acked);

  stream->bbr.ack_epoch_acked += rs->acked_sacked;

  uint32_t extra_acked = stream->bbr.ack_epoch_acked - expected_acked;
  extra_acked = min_uint32(extra_acked, stream->cwnd);

  if (extra_acked > stream->bbr.extra_acked[stream->bbr.extra_acked_win_index]) {
    stream->bbr.extra_acked[stream->bbr.extra_acked_win_index] = extra_acked;
  }
}

// estimate when the pipe is full, using the change in delivery rate. BBR
// estimates that STARTUP filled the pipe if the estmated bw hasn't chnged by at least 25%
// after 3 non-app-limited rounds.
// 3 rounds is chosen because
// 1. rwin autotuning grows the rwin (this applies to Linux TCP, not to us though. our RWIN is not auto-tuned)
// 2. we fill the higher rwin
// 3. we get higher delivery rate samples.
// also, transient cross-traffic or radio noise can go away, CUBIC HyStart has
// a similar design goal, but uses delay and inter-ack spacing instead of bandwidth

static void
bbr_check_full_bw_reached (udx_stream_t *stream, udx_rate_sample_t *rs) {
  // BBR 4.3.1.2
  if (bbr_full_bw_reached(stream) || !stream->bbr.round_start || rs->is_app_limited)
    return;

  double bw_thresh = stream->bbr.full_bw * bbr_full_bw_thresh;

  if (bbr_max_bw(stream) >= bw_thresh) {
    stream->bbr.full_bw = bbr_max_bw(stream);
    stream->bbr.full_bw_count = 0;
    return;
  }

  ++stream->bbr.full_bw_count;
  stream->bbr.full_bw_reached = stream->bbr.full_bw_count >= bbr_full_bw_count;
  if (stream->bbr.full_bw_reached) {
    debug_throughput_printf(stream, "full_bw_reached");
  }
}

static void
bbr_check_drain (udx_stream_t *stream, udx_rate_sample_t *rs) {
  UDX_UNUSED(rs);
  if (stream->bbr.state == UDX_BBR_STATE_STARTUP && bbr_full_bw_reached(stream)) {
    // BBR 4.3.1.1

    debug_throughput_printf(stream, "drain");
    stream->bbr.state = UDX_BBR_STATE_DRAIN; // drain hypothetical bottleneck queue
    stream->ssthresh = bbr_inflight(stream, bbr_max_bw(stream), 1.0);
    // debug_printf("bbr: rid=%u entering DRAIN cwnd=%u ssthresh=%u bbr_max_bw=%f time=%lu\n", stream->remote_id, stream->cwnd, stream->ssthresh, bbr_max_bw(stream), uv_now(stream->udx->loop));
  }

  if (stream->bbr.state == UDX_BBR_STATE_DRAIN && stream->inflight_queue.len <= bbr_inflight(stream, bbr_max_bw(stream), 1.0)) {
    bbr_reset_probe_bw_mode(stream); // estimate that queue is drained
  }
}

int64_t
time_diff (uint64_t a, uint64_t b) {
  return a - b;
}

static void
bbr_check_probe_rtt_done (udx_stream_t *stream, uint64_t now_ms) {
  if (!(stream->bbr.probe_rtt_done_time && time_diff(now_ms, stream->bbr.probe_rtt_done_time) > 0)) {
    return;
  }
  debug_throughput_printf(stream, "probe_rtt_done");
  stream->bbr.min_rtt_stamp = now_ms;
  stream->cwnd = max_uint32(stream->cwnd, stream->bbr.prior_cwnd);
  bbr_reset_mode(stream);
}

// The goal of UDX_BBR_STATE_PROBE_RTT is to have BBR flows cooperatively and periodically drain the bottleneck
// queue, to converge to measure the true min_rtt (unloaded propagation delay). this allows the flows
// to keep queues small (reducing delay and packet loss) and achieve fairness among he bbr flows

// the min rtt filter window is 10 seconds. when the min_rtt estimate expires, we enter UDX_BBR_STATE_PROBE_RTT
// and cap the cwnd at bbr_cwnd_min_target = 4;
// after at least 200ms and at least one packet timed round trip elapsed with that flight size <= 4, we leave
// UDX_BBR_STATE_PROBE_RTT and re-enter the previous mode. BBR uses 200ms to approximately bound the performance
// penalty of PROBE_RTT's cwnd capping to roughly 2% (200ms / 10s)

// note that this penalty is only paid if they are actually busy sending the full 10 seconds. we pick up
// these min RTT measurements opportunistically with the min_RTT filter. (-:
static void
bbr_update_min_rtt (udx_stream_t *stream, udx_rate_sample_t *rs) {
  bool filter_expired;

  uint64_t now = uv_now(stream->udx->loop); // may want to cache current time on each stream

  filter_expired = time_diff(now, stream->bbr.min_rtt_stamp + UDX_BBR_MIN_RTT_INTERVAL_MS) > 0;

  if (rs->rtt_ms >= 0 && (rs->rtt_ms < (int64_t) stream->bbr.min_rtt_ms || (filter_expired && !rs->is_ack_delayed))) {
    // round up to 1ms, since our timer has 1ms resolution
    stream->bbr.min_rtt_ms = rs->rtt_ms ? rs->rtt_ms : 1;
    stream->bbr.min_rtt_stamp = now;
  }

  if (UDX_BBR_MIN_PROBE_RTT_MODE_MS > 0 && filter_expired && !stream->bbr.idle_restart && stream->bbr.state != UDX_BBR_STATE_PROBE_RTT) {
    // debug_printf("bbr: rid=%u entering PROBE_RTT time=%ld\n", stream->remote_id, now);
    debug_throughput_printf(stream, "probe_rtt");
    stream->bbr.state = UDX_BBR_STATE_PROBE_RTT;
    bbr_save_cwnd(stream);
    stream->bbr.probe_rtt_done_time = 0;
  }

  if (stream->bbr.state == UDX_BBR_STATE_PROBE_RTT) {

    stream->app_limited = (stream->delivered + stream->inflight_queue.len) ?: 1;

    if (!stream->bbr.probe_rtt_done_time && stream->inflight_queue.len <= bbr_cwnd_min_target) {
      stream->bbr.probe_rtt_done_time = now + UDX_BBR_MIN_PROBE_RTT_MODE_MS; // 200 ms
      stream->bbr.probe_rtt_round_done = false;
      stream->bbr.next_rtt_delivered = stream->delivered;
    } else if (stream->bbr.probe_rtt_done_time) {
      if (stream->bbr.round_start)
        stream->bbr.probe_rtt_round_done = true;
      if (stream->bbr.probe_rtt_round_done)
        bbr_check_probe_rtt_done(stream, now);
    }
  }

  if (rs->delivered > 0) {
    stream->bbr.idle_restart = false;
  }
}

static void
bbr_update_gains (udx_stream_t *stream) {
  switch (stream->bbr.state) {
  case UDX_BBR_STATE_STARTUP:
    // 4.3.1.1
    stream->bbr.pacing_gain = bbr_high_gain;
    stream->bbr.cwnd_gain = bbr_high_gain; // linux kernel uses 2.89, IETF draft uses 2. both work fine
    break;
  case UDX_BBR_STATE_DRAIN:
    stream->bbr.pacing_gain = bbr_drain_gain;
    stream->bbr.cwnd_gain = bbr_high_gain; // bbrv2 uses default_cwnd_gain (2)
    break;
  case UDX_BBR_STATE_PROBE_BW:
    stream->bbr.pacing_gain = bbr_pacing_gain[stream->bbr.cycle_index];
    stream->bbr.cwnd_gain = bbr_cwnd_gain;
    break;
  case UDX_BBR_STATE_PROBE_RTT:
    stream->bbr.pacing_gain = 1.0;
    stream->bbr.cwnd_gain = 1.0;
    break;
  default:
    assert(0 && "bad bbr mode");
  }
}

static void
bbr_update_model (udx_stream_t *stream, udx_rate_sample_t *rs) {
  bbr_update_bw(stream, rs);
  bbr_update_ack_aggregation(stream, rs);
  bbr_update_cycle_phase(stream, rs);
  bbr_check_full_bw_reached(stream, rs);
  bbr_check_drain(stream, rs);
  bbr_update_min_rtt(stream, rs);
  bbr_update_gains(stream);
}

// This is the first of BBR functions that are clients of udx.c
// bbr_main(stream, nacked, int flag, udx_rate_sample_t rs)
//     called on each processed ack with new rate sample generated
void
bbr_main (udx_stream_t *stream, udx_rate_sample_t *rs) {
  // BBR 4.2.3
  bbr_update_model(stream, rs);

  double bw = bbr_bw(stream);
  bbr_set_pacing_rate(stream, bw, stream->bbr.pacing_gain);
  bbr_set_cwnd(stream, rs, rs->acked_sacked, bw, stream->bbr.cwnd_gain);
}

void
bbr_init (udx_stream_t *stream) {
  stream->bbr.prior_cwnd = 0;
  stream->ssthresh = 0xffff;
  stream->bbr.rtt_count = 0;
  stream->bbr.next_rtt_delivered = stream->delivered;
  stream->bbr.prev_ca_state = UDX_CA_OPEN;
  stream->bbr.use_packet_conservation = false;

  stream->bbr.probe_rtt_done_time = 0;
  stream->bbr.probe_rtt_round_done = 0;
  stream->bbr.min_rtt_ms = udx_rtt_min(stream);
  stream->bbr.min_rtt_stamp = uv_now(stream->udx->loop);

  win_filter_f64_reset(&stream->bbr.bw, stream->bbr.rtt_count, 0);

  stream->bbr.has_seen_rtt = false;
  bbr_init_pacing_rate_from_rtt(stream);

  stream->bbr.round_start = false;
  stream->bbr.idle_restart = false;
  stream->bbr.full_bw_reached = 0;
  stream->bbr.full_bw = 0.0;
  stream->bbr.full_bw_count = 0;
  stream->bbr.cycle_timestamp = 0;
  stream->bbr.cycle_index = 0;
  bbr_reset_startup_mode(stream);

  stream->bbr.ack_epoch_start = uv_now(stream->udx->loop);
  stream->bbr.ack_epoch_acked = 0;
  stream->bbr.extra_acked_win_rtts = 0;
  stream->bbr.extra_acked_win_index = 0;
  stream->bbr.extra_acked[0] = 0;
  stream->bbr.extra_acked[1] = 0;
}

// Linux additionally adds these hooks which we may consider in the future
// bbr_sndbuf_expand(socket)
//   - unlikely to need, we limit ourselves only with cwnd and rwnd, but don't have a memory limit
//     (besides of course the kernels send buffer)
// bbr_undo_cwnd(socket)
//   - used for cwnd reduction undo e.g. with DSACK
// bbr_ssthresh(socket)
//   - called when entering loss recovery, used to save cwnd for recovery

uint32_t
bbr_ssthresh (udx_stream_t *stream) {
  bbr_save_cwnd(stream);
  return stream->ssthresh;
}

int
udx_stream_get_bw (udx_stream_t *stream, uint64_t *bw_bytes_per_sec_out) {
  double bw_packets_per_ms = bbr_bw(stream); // packets / ms
  uint32_t mss = udx__max_payload(stream);   // excludes header size, ie the value returned is bandwidth available for payload

  *bw_bytes_per_sec_out = bw_packets_per_ms * 1000 * mss;

  return 0;
}

int
udx_stream_get_min_rtt (udx_stream_t *stream, uint32_t *min_rtt_ms) {
  *min_rtt_ms = stream->bbr.min_rtt_ms;

  return 0;
}

void
bbr_on_rto (udx_stream_t *stream) {
  // BBR 4.2.4
  // simulate a dummy loss rate sample when we hit RTO
  stream->bbr.prev_ca_state = UDX_CA_LOSS;
  stream->bbr.full_bw = 0.0;
  stream->bbr.round_start = true;
}

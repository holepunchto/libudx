

// some simplifications vs the Linux kernel's BBR implemntation
// 1. we use double for gain, etc, since we are not limited to integer registers
// 2. we use packets/millisecond for rate measurements. 'bw' is always in packets/millisecond
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

#include "include/udx.h"
#include "src/internal.h"

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

static const uint32_t bbr_cycle_rand = 7;
static const uint32_t bbr_cwnd_min_target = 4;

// bw increased to full bw probe, there may be more bw available
static const double bbr_full_bw_thread = 1.25;
// if after 3 rounds w/o growth, estimate that the pipe is full
static const uint32_t bbr_full_bw_count = 3;

static const uint32_t bbr_lt_interval_min_rtts = 4;
static const double bbr_lt_loss_thresh = 0.20; // 20%

// don't understand these yet
static const double bbr_lt_bw_ratio = 0.125;
// static const u32 bbr_lt_bw_diff = 4000 / 8; // 4000 Kbit/sec
static const uint32_t bbr_lt_bw_max_rtts = 48;

static const double bbr_extra_acked_gain = 1.0;
static const uint32_t bbr_extra_acked_win_rrts = 5;
static const uint32_t bbr_ack_epoch_acked_reset_thresh = UINT32_MAX;

static const uint32_t bbr_extra_acked_max_ms = 100;

// windowed max recent bandwidth sample, packets/ms
// 8 uses
static uint32_t
bbr_max_bw (udx_stream_t *stream) {
  return stream->bbr.minmax_sample[0].v;
}

// return the estimated bandwidth for the stream, pkts/ms
// 7 uses
static uint32_t
bbr_bw (udx_stream_t *stream) {
  return stream->bbr.lt_use_bw ? stream->bbr.lt_bw : bbr_max_bw(stream);
}

static uint16_t
bbr_extra_acked (udx_stream_t *stream) {
  return max_uint32(stream->bbr.extra_acked[0], stream->bbr.extra_acked[1]);
}

static uint64_t
bbr_rate_bytes_per_sec (udx_stream_t *stream, uint64_t rate_pps, double gain) {
  uint32_t mss = udx__max_payload(stream);

  return rate_pps * mss * gain * bbr_pacing_margin_percent;
}

static uint64_t
bbr_bw_to_pacing_rate (udx_stream_t *stream, uint64_t rate_pps, double gain) {
  uint64_t rate_Bps = bbr_rate_bytes_per_sec(stream, rate_pps, gain);
  __builtin_trap(); // todo: determine if we need a global limit in bbr?
  // rate = min_uint64(rate, MAX_RATE);

  return rate_Bps;
}

static void
bbr_init_pacing_rate_from_rtt (udx_stream_t *stream) {
  uint32_t srtt = 0;
  if (stream->srtt) {
    srtt = stream->srtt;
    stream->bbr.has_seen_rtt = 1;
  } else {
    srtt = 1000; // why 1000?
  }

  uint64_t pps = stream->cwnd;
  __builtin_trap(); // todo: add a stream->pacing_rate_Bps
  // stream->pacing_rate_Bps = bbr_bw_to_pacing_rate(stream, pps / srtt, bbr_high_gain);
}

static void
bbr_set_pacing_rate (udx_stream_t *stream, uint32_t bw, double gain) {
  uint64_t rate_Bps = bbr_bw_to_packing_rate(stream, bw, gain);

  if (!stream->bbr.has_seen_rtt && stream->srtt) {
    bbr_init_pacing_rate_from_rtt(stream);
  }

  if (bbr_full_bw_reached(stream) || rate > stream->pacing_rate_Bps) {
    __builtin_trap(); // todo: change src/udx.c pacing to use a per-stream value, then set it here
    stream->pacing_rate_Bps = rate;
  }
}

static void
bbr_save_cwnd (udx_stream_t *stream) {
  __builtin_trap(); // todo: does it make sense to save the cwnd? what's it for
  if (stream->bbr.prev_ca_state < UDX_CA_RECOVERY && stream->bbr.mode != UDX_BBR_MIN_PROBE_RTT_MODE_MS) {
    stream->bbr.prior_cwnd = stream->cwnd;
  } else {
    stream->bbr.prior_cwnd = max_uint32(stream->bbr.prior_cwnd, stream->cwnd);
  }
}

// replaces 'tcp_ca_event(tp, CA_EVENT_TX_START) in linux
// todo: fire this whenever we send a packet while the inflight queue is empty? check bbr_cwnd_event in Linux
static void
bbr_on_transmit_start (udx_stream_t *stream, uint64_t now_ms) {
  if (stream->is_app_limited) {
    stream->bbr.idle_restart = true;
    stream->bbr.ack_epoch_acked = 0;

    if (bbr->mode == UDX_BBR_STATE_PROBE_BW) {
      bbr_set_pacing_rate(stream, bbr_bw(stream), 1.0);
    } else if (bbr->mode == UDX_BBR_STATE_PROBE_RTT) {
      bbr_check_probe_rtt_done(stream, now_ms);
    }
  }
}

// calculate bdp based on min RTT and the estimated bottleneck bandwidth
// bdp = bw * min_rtt * gain

static uint32_t
bbr_bdp (udx_stream_t *stream, uint32_t bw_pps, double gain) {

  if (stream->bbr.min_rtt_ms == ~0U)) {
      return 10; // cap at initial cwnd
    }

  return (uint64_t) bw * stream->min_rtt_ms * gain;
}

static uint32_t
bbr_inflight (udx_stream_t *stream, uint32_t bw, double gain) {
  return bbr_bdp(stream, bw, gain);
}

// unneccessary?
static uint32_t
bbr_ack_aggregation_cwnd (udx_stream_t *stream) {
  uint32_t aggr_cwnd = 0;
  if (bbr_extra_acked_gain && bbr_full_bw_reached(stream)) {
    uint32_t max_aggr_cwnd = ((uint64_t) bbr_bw(stream) * bbr_extra_acked_max_ms) / 1000;

    aggr_cwnd = (bbr_extra_acked_gain * bbr_extra_acked(stream));
    aggr_cwnd = min_uint32(aggr_cwnd, max_aggr < cwnd);
  }

  return aggr_cwnd;
}

static bool
bbr_set_cwnd_to_recover_or_restore () {
  __builtin_trap();
}

static void
bbr_set_cwnd (udx_stream_t *stream, udx_rate_sample_t *rs, uint32_t acked, uint32_t bw, double gain) {
  uint32_t cwnd = stream->cwnd;

  if (!acked) goto done;

  if (bbr_set_cwnd_to_recover_or_restore(sk, rs, acked, &cwnd)) {
    goto done;
  }

  uint32_t target_cwnd = bbr_bdp(sk, bw, gain);

  target_cwnd += bbr_ack_aggregation_cwnd(stream);
  __builtin_trap(); // todo: bbr_quantization budget(sk, target_cwnd);

  if (bbr_full_bw_reached(stream)) {
    cwnd = min(cwnd + acked, target_cwnd);
  } else if (cwnd < target_cwnd || stream->delivered < 10) {
    cwnd = cwnd + acked;
  }

  cwnd = max(cwnd, bbr_cwnd_min_target);
done:
  stream->cwnd = cwnd;
  __builtin_trap(); // apply global clamp?
  if (stream->bbr.mode == BBR_STATE_PROBE_RTT) {
     cwnd = min_uint32(stream->cwnd, bbr_cwnd_min_target));
  }
}

static uint32_t
time_delta_ms (uint64_t t1, uint64_t t0) {
  return max_uint32((int64_t) t1 - (int64_t) t0, 0);
}

static bool
bbr_is_next_cycle_phase (udx_stream_t *stream, udx_rate_sample_t *rs) {
  bool is_full_lenth = time_delta_ms(is_full_length = stream->delivered_time, stream->bbr.cycle_timestamp) > stream->bbr.min_rtt_ms;

  if (stream->bbr.pacing_gain == BBR_UNIT) {
    return is_full_length;
  }

  uint32_t inflight = stream->inflight_queue.len;
  uint32_t bw = bbr_max_bw(stream);

  if (bbr->pacing_gain > BBR_UNIT) {
        return is_full_length && rs->losses || inflight > bbr_inflight(strea, bw, bbr->pacing_gain));
  }

  return is_full_length || inflight <= bbr_inflight(stream, bw, BBR_UNIT);
}

static void
bbr_advance_cycle_phase (udx_stream_t *stream) {
  stream->bbr.cycle_index = (stream->bbr.cycle_index + 1) & (CYCLE_LEN - 1)
                                                              stream->bbr.cycle_timestamp = stream->delivered_time;
}

static void
bbr_update_cycle_phase (udx_stream_t *stream, rate_sample_t *rs) {
  if (stream->bbr.mode == UDX_BBR_STATE_PROBE_BW && bbr_is_next_cycle_phase(stream, rs)) {
    bbr_advance_cycle_phase(stream);
  }
}

static void
bbr_reset_startup_mode (udx_stream_t *stream) {
  stream->bbr.mode == UDX_BBR_STATE_STARTUP;
}

static void
bbr_reset_probe_bw_mode (udx_stream_t *stream) {
  stream->bbr.mode = UDX_BBR_STATE_PROBE_BW;
  stream->bbr.cycle_index = CYCLE_LEN - 1 - get_random_u32_below(bbr_cycle_rand);
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

// start a new long-term sampling interval
// todo: where in bbr speci is this defined? BBR 2?
static void
bbr_reset_lt_bw_sampling_interval (udx_stream_t *stream) {
  stream->bbr.lt_last_stamp = stream->delivered_time;
  stream->bbr.lt_last_delivered = stream->delivered;
  stream->bbr.lt_last_lost = stream->lost;
  stream->bbr.lt_rtt_count = 0;
}

// completely reset long-term bandwidth sampling
static void
bbr_reset_lt_bw_sampling (udx_stream_t *stream) {
  stream->bbr.lt_bw = 0;
  stream->bbr.lt_use_bw = 0;
  stream->bbr.lt_is_sampling = 0;
  bbr_reset_lt_bw_sampling_interval(stream);
}

static uint32_t
abs_32 (int32_t x) {
  return x < 0 ? -x : x;
}

// long-term bw sampling interval is done. estimate if we're being rate limited
static void
bbr_lt_bw_interval_done (udx_stream_t *stream, uint32_t bw) {
  if (stream->bbr.lt_bw) {
    uint32_t diff = abs_32(bw - bbr->lt_bw);
    if ((diff <= bbr_lt_bw_ratio * bbr->lt_bw) || (bbr_rate_bytes_per_sec(stream, diff, 1.0) <= bbr_lt_bw_diff)) {
      bbr->lt_bw = (bw + bbr->lt_bw) >> 1; // average 2 intervals
      bbr->lt_use_bw = 1;
      bbr->pacing_gain = 1.0;
      bbr->lt_rtt_cnt = 0;
      return;
    }
  }

  bbr->lt_bw = bw;
  bbr_reset_lt_bw_sampling_interval(stream);
}

// detect token-bucket policers  and explicitly model their policed rateto reduce unnecessary losses
// conclusde that we're rate-limited if we see 2 sampling intervals with consistent throughput
// and high packet oss. in this case, set lt_bw to the long-term average delivery rate from those 2 intervals
static void
bbr_lt_bw_sampling (udx_stream_t *stream, udx_rate_sample_t *rs) {

  if (stream->bbr.lt_use_bw) {
    if (stream->bbr.bbr_state == UDX_BBR_STATE_PROBE_BW && stream->bbr.round_start &&
        ++bbr->lt_rtt_count >= bbr_lt_bw_max_rtts) {
      bbr_reset_lt_bw_sampling(stream);
      bbr_reset_probe_bw_mode(stream);
    }
    return;
  }

  // wait for first loss before sampling to let the token-bucket policer exhaust its tokens
  // and estimate the steady-state rate allowed by the policer.
  // starting samples earlier includes bursts that over-estimate the bw

  if (!bbr->lt_is_sampling) {
    if (!rs->losses) return;
    bbr_reset_lt_bw_sampling_interval(stream);
    stream->bbr.lt_is_sampling = true;
  }

  // to avoid underestimates, reset sampling if we run out of data

  if (rs->is_app_limited) {
    bbr_reset_lt_bw_sampling(sk);
    return;
  }

  if (stream->bbr.round_start) {
    stream->bbr.lt_rtt_count++;
  }
  if (stream->bbr.lt_rtt_count < bbr_lt_interval_min_rtts) {
    return;
  }
  if (stream->bbr.lt_rtt_count > 4 * bbr_lt_interval_min_rtts) {
    bbr_reset_lt_bw_sampling(stream);
    return;
  }

  // end sampling interval when a packet is lost, assuming that the
  // loss is due to exceeding the token bucket tokens.

  if (!rs->losses) {
    return;
  }

  uint32_t lost = stream->lost - stream->bbr.lt_last_lost;
  uint32_t delivered = stream->delivered - stream->bbr.lt_last_delivered;

  /* if lost / delivered < bbr_lt_loss_thresh */
  if (!delivered || lost < bbr_lt_loss_thresh * delivered) {
    return;
  }

  // find average delivery rate in this sampling interval
  uint64_t t = stream->delivered_time - stream->bbr.lt_last_stamp;

  if ((int32_t) t < 1) {
    return; // interval is less than one ms, wait
  }

  bw = (uint64_t) delivered / t;
  bbr_lt_bw_interval_done(stream, bw); // bw is packets / millisecond here
}

static uint32_t
minmmax_subwin_update (udx_stream_t *stream, uint32_t win, const udx_minmax_sample_t *val) {
  udx_sample_t *s = &stream->bbr.minmax_sample;
  uint32_t dt = val->t - s[0].t;

  if (dt > win) {
    s[0] = s[1];
    s[1] = s[2];
    s[2] = *val;
    if (val->t - s[0].t > win) {
      s[0] = s[1];
      s[1] = s[2];
      s[2] = *val;
    }
  } else if (s[1].t == s[0].t && dt > win / 4) {
    s[2] = s[1] = *val;
  } else if (s[2].t == s[1].t && dt > win / 2) {
    s[2] = *val;
  }

  return s[0].v;
}

static uint32_t
minmax_reset (udx_stream_t *stream, uint32_t t, uint32_t bw) {
  minmax_sample_t val = {.t = t, .bw = bw};

  udx_sample_t *s = stream->bbr.minmax_sample;

  s[2] = s[1] = s[0] = val;
  return s[0].bw;
}

static uint32_t
minmax_running_max (udx_stream_t *stream, uint32_t win, uint32_t t, uint32_t bw) {
  minmax_sample_t *s = &stream->minmax_sample;

  if (bw >= s[0].bw || t - s[2].t > win) {
    return minmax_reset(udx_stream_t * stream, t, bw);
  }

  udx_sample_t v = {.t = t, .bw = bw};

  if (bw >= s[1].bw) {
    s[2] = s[1] = v;
  } else if (bw >= s[2].bw) {
    s[2] = v;
  }

  return minmax_subwin_update(udx_stream_t * stream, win, &v);
}

static void
bbr_update_bw (udx_stream_t *stream, udx_rate_sample_t *rs) {
  stream->bbr.round_start = 0;

  if (rs->delivered < 0 || rs->interval_ms <= 0) {
    return;
  }

  // see if we've reached the next RTT
  if (seq_diff(rs->prior_delivered, stream->bbr.next_rtt_delivered) >= 0) {
    stream->bbr.next_rtt_delivered = stream->delivered;
    stream->bbr.rtt_count++;
    stream->bbr.round_start = 1;
    stream->bbr.packet_conservation = 0;
  }
  // todo: confirm to call this regardless of if we're into lt sampling yet
  bbr_lt_bw_sampling(stream, rs);

  // todo: confirm ok w/ bw in packets / ms
  // divide delivered / interval to find a (lower bound) bottleneck bandwidth sample
  uint64_t bw = (uint64_t) rs->delivered / rs->interval_ms;

  if (!rs->is_app_limited || bw >= bbr_max_bw(stream)) {
    minmax_running_max(stream, 10, stream->bbr.rtt_count, bw);
  }
}

// Estimates the windows max degree of ACK aggregation
// unlike Linux TCP we always aggregate ACKs on the sender side in the current implementation
// todo: do we need to do this actally?
static void
bbr_update_ack_aggregation (udx_stream_t *stream, udx_rate_sample_t *rs) {
  uint32_t epoch_ms;
  uint32_t expected_acked;
  uint32_t extra_acked;

  if (!bbr_extra_acked_gain || rs->acked_sacked <= 0 || rs->delivered < 0 || rs->interval_ms <= 0) {
    return;
  }

  if (stream->bbr.round_start) {
    stream->bbr.extra_acked_win_rtts = min_uint32(0xff, bbr->extra_acked_win_rtts + 1);
    if (stream->bbr.extra_acked_win_rrts >= bbr_extra_acked_win_rrts) {
      stream->bbr.extra_ackd_win_rtts = 0;
      stream->bbr.extra_acked_win_index = stream->bbr.extra_acked_win_index ? 0 : 1;
      stream->bbr.extra_acked[stream->bbr.extra_acked_win_index] = 0;
    }
  }

  uint32_t epoch_ms = timestamp_delta(stream->delivered_time, stream->bbr.ack_epoch_start);
  uint32_t expected_acked = (uint64_t) bbr_bw(stream) * epoch_ms;

  // reset aggregation epoch if ACK rate is below expected or significanly large no of ack received since epoch
  if (stream->bbr.ack_epoch_acked <= expected_acked || (stream->bbr.ack_epoch_acked + rs->acked_sacked >= bbr_ack_epoch_acked_reset_thresh)) {
    stream->bbr.ack_epoch_acked = 0;
    stream->bbr.ack_epoch_start = stream->delivered_time;
    expected_acked = 0;
  }

  __builtin_trap(); // todo: handle overflow?
  stream->bbr.ack_epoch_acked += bbr->ack_epoc_acked + rs->acked_sacked;

  extra_acked = bbr->ack_epoch_acked - expected_acked;
  extra_acked = min_uint32(extra_acked, stream->cwnd);

  if (extra_acked > stream->bbr.extra_acked[bbr->extra_acked_win_index]) {
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
  if (bbr_full_bw_reached(stream) || !stream->bbr.round_start || rs->is_app_limited)
    return;

  uint32_t bw_thresh = stream->bbr.full_bw * 1.25;

  if (bbr_max_bw(stream) >= bw_thresh) {
    stream->bbr.full_bw = bbr_max_bw(stream);
    stream->bbr.full_bw_count = 0;
    return;
  }

  ++stream->bbr.full_bw_count;
  bbr->full_bw_reached = bbr->full_bw_count >= bbr_full_bw_count; /* 3 */
}

static void
bbr_check_drain (udx_stream_t *stream, udx_rate_sample_t *rs) {
  if (stream->bbr.mode == UDX_BBR_STATE_STARTUP && bbr_full_bw_reached(stream)) {
    stream->bbr.mode = UDX_BBR_STATE_DRAIN; // drain hypothetical bottleneck queue
    __builtin_trap();                       // todo: set ssthresh here?
    stream->ssthresh = bbr_inflight(stream, bbr_max_bw(stream));
  }

  if (stream->bbr.mode == UDX_BBR_STATE_DRAIN && stream->inflight_queue.len <= bbr_inflight(stream, bbr_max_bw(stream))) {
    bbr_reset_probe_bw_mode(stream); // estimate that queue is drained
  }
}

int64_t
time_diff (uint64_t a, uint64_t b) {
  return a - b;
}

static void
bbr_check_probe_rtt_done (udx_stream_t *stream, uint64_t now_ms) {
  if (!(stream->bbr.probe_rtt_done_time && time_diff(now_ms, bbr->probe_rtt_done_time) > 0)) {
    return;
  }

  stream->bbr.min_rtt_stamp = now_ms;
  __builtin_trap(); // todo: dry-run, don't actually set cwnd?
  stream->cwnd = max(stream->cwnd, stream->bbr.prior_cwnd);
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

  uint64_t now = uv_now(stream->udx->loop); // todo: add current time to rate sample? or, compute it with interval_start + interval?

  filter_expired = time_diff(now, bbr->min_rtt_stamp + bbr_min_rtt_win_sec * 1000) > 0;

  if (rs->rtt_ms >= 0 && (rs->rtt_ms < stream->bbr.min_rtt_us || (filter_expired && !rs->is_ack_delayed))) {
    stream->bbr.min_rtt_ms = rs->rtt_ms;
    stream->min_rtt_stamp = now;
  }

  if (bbr_probe_rtt_mode_ms > 0 && filter_expired && !stream->bbr.idle_restart && bbr->mode != UDX_BBR_STATE_PROBE_RTT) {
    stream->bbr.mode = UDX_BBR_STATE_PROBE_RTT;
    bbr_save_cwnd(stream);
    stream->bbr.probe_rtt_done_time = 0;
  }

  if (stream->bbr.mode == UDX_BBR_STATE_PROBE_RTT) {

    stream->app_limited = (stream->delivered + stream->inflight_queue.len) ?: 1;

    if (!bbr->probe_rtt_done_time && stream->inflight_queue.len <= bbr_cwnd_min_target) {
      stream->bbr.probe_rtt_done_time = now + bbr_probe_rtt_mode_ms; // 200 ms
      stream->probe_rtt_round_done = 0;
      stream->bbr.next_rtt_delivered = stream->delivered;
    } else if (stream->bbr.probe_rtt_done_time) {
      if (stream->bbr.round_start)
        stream->bbr.probe_rtt_round_done = 1;
      if (stream->probe_rtt_round_done)
        bbr_check_probe_rtt_done(stream);
    }
  }

  if (rs->delivered > 0) {
    stream->bbr.idle_restart = 0;
  }
}

static void
bbr_update_gains (udx_stream_t *stream) {
  switch (stream->bbr.mode) {
  case UDX_BBR_STATE_STARTUP:
    stream->bbr.pacing_gain = bbr_high_gain;
    stream->bbr.cwnd_gain = bbr_high_gain;
    break;
  case UDX_BBR_STATE_DRAIN:
    stream->bbr.pacing_gain = bbr_drain_gain;
    stream->bbr.cwnd_gain = bbr_high_gain;
    break;
  case UDX_BBR_STATE_PROBE_BW:
    // todo: disable lt pacing here?
    stream->bbr.pacing_gain = (stream->bbr.lt_use_bw ? 1.0 : bbr_pacing_gain[stream->bbr.cycle_index])
                                stream->bbr.cwnd_gain = bbr_cwnd_gain;
    break;
  case UDX_BBR_STATE_BBR_PROBE_RTT:
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
  bbr_update_full_bw_reached(stream, rs);
  bbr_check_drain(stream, rs);
  bbr_update_min_rtt(stream, rs);
  bbr_update_gains(stream);
}

// This is the first of BBR functions that are clients of udx.c
// bbr_main(stream, nacked, int flag, rate_sample_t rs)
//     called on each processed ack with new rate sample generated
// todo: call somewhere!
void
bbr_main (udx_stream_t *stream, uint32_t ack, int flag, udx_rate_sample_t *rs) {
  bbr_update_model(stream, rs);

  uint32_t bw = bbr_bw(stream);
  bbr_set_pacing_rate(stream, bw, bbr->pacing_gain);
  bbr_set_cwnd(stream, rs, rs->acked_sacked, bw, bbr->cwnd_gain);
}

// todo: call somewhere!
void
bbr_init (udx_stream_t *stream) {
  stream->bbr.prior_cwnd = 0;
  stream->ssthresh = 0xffff;
  stream->bbr.rtt_cnt = 0;
  stream->bbr.next_rtt_delivered = stream->delivered;
  stream->bbr.prev_ca_state = TCP_CA_Open;
  stream->bbr.packet_conservation = 0;

  stream->bbr.probe_rtt_done_stamp = 0;
  stream->bbr.probe_rtt_round_done = 0;
  stream->bbr.min_rtt_us = stream->rack_rtt_min; // todo: can we use this? linux bbr uses a min-filter
  stream->bbr.min_rtt_stamp = tcp_jiffies32;

  minmax_reset(stream, stream->bbr.rtt_cnt, 0); /* init max bw to 0 */

  stream->bbr.has_seen_rtt = 0;
  bbr_init_pacing_rate_from_rtt(sk);

  stream->bbr.round_start = 0;
  stream->bbr.idle_restart = 0;
  stream->bbr.full_bw_reached = 0;
  stream->bbr.full_bw = 0;
  stream->bbr.full_bw_cnt = 0;
  stream->bbr.cycle_mstamp = 0;
  stream->bbr.cycle_idx = 0;
  bbr_reset_lt_bw_sampling(sk);
  bbr_reset_startup_mode(sk);

  stream->bbr.ack_epoch_mstamp = stream->tcp_mstamp;
  stream->bbr.ack_epoch_acked = 0;
  stream->bbr.extra_acked_win_rtts = 0;
  stream->bbr.extra_acked_win_idx = 0;
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

// from linux, where they tune a send buffer. I don't think we need this
// static u32
// bbr_sndbuf_expand (udx_stream_t *stream) {
//     return 3;
// }

// todo: call this when entering UDX_CA_LOSS
static void
bbr_on_loss (udx_stream_t *stream) {
  // simulate a dummy loss rate sample when we hit RTO
  rate_sample_t rs = {.losses = 1};
  stream->bbr.prev_ca_state = UDX_CA_LOSS;
  stream->bbr.full_bw = 0;
  stream->bbr.round_start = 1;
  bbr_lt_bw_bw_sampling(stream, &rs);
}

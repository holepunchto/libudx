/*
 * Copyright 2017, Google Inc.
 *
 * Use of this source code is governed by the following BSD-style license:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *    * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *
 *    * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/**
 * lib/minmax.c: windowed min/max tracker
 *
 * Kathleen Nichols' algorithm for tracking the minimum (or maximum)
 * value of a data stream over some fixed time interval.  (E.g.,
 * the minimum RTT over the past five minutes.) It uses constant
 * space and constant time per update yet almost always delivers
 * the same minimum as an implementation that has to keep all the
 * data in the window.
 *
 * The algorithm keeps track of the best, 2nd best & 3rd best min
 * values, maintaining an invariant that the measurement time of
 * the n'th best >= n-1'th best. It also makes sure that the three
 * values are widely separated in the time window since that bounds
 * the worse case error when that data is monotonically increasing
 * over the window.
 *
 * Upon getting a new min, we can forget everything earlier because
 * it has no value - the new min is <= everything else in the window
 * by definition and it's the most recent. So we restart fresh on
 * every new min and overwrites 2nd & 3rd choices. The same property
 * holds for 2nd & 3rd best.
 *
 */

#include "win_filter.h"

static uint32_t
win_filter_apply_common (win_filter_t *wf, uint32_t win, uint64_t t, uint32_t v) {
  uint32_t dt = t - wf->entries[0].t;

  win_filter_entry_t new = {.t = t, .v = v};

  if (dt > win) {
    // we've passed the window so bump off entries[0]
    wf->entries[0] = wf->entries[1];
    wf->entries[1] = wf->entries[2];
    wf->entries[2] = new;
    if (t - wf->entries[0].t > win) {
      // bump off entries[1] too
      wf->entries[0] = wf->entries[1];
      wf->entries[1] = wf->entries[2];
      wf->entries[2] = new;
    }
  } else if (wf->entries[1].t == wf->entries[0].t && dt > win / 4) {
    wf->entries[2] = new;
    wf->entries[1] = new;
  } else if (wf->entries[2].t == wf->entries[1].t && dt > win / 2) {
    wf->entries[2] = new;
  }

  return wf->entries[0].v;
}

uint32_t
win_filter_reset (win_filter_t *wf, uint64_t t, uint32_t v) {
  win_filter_entry_t val = {.t = t, .v = v};

  wf->entries[2] = wf->entries[1] = wf->entries[0] = val;
  return wf->entries[0].v;
}

uint32_t
win_filter_apply_max (win_filter_t *wf, uint32_t win, uint64_t t, uint32_t v) {
  //  new maximum           || nothing in window
  if (v >= wf->entries[0].v || t - wf->entries[2].t > win) {
    return win_filter_reset(wf, t, v);
  }

  win_filter_entry_t new = {.t = t, .v = v};

  if (v >= wf->entries[1].v) {
    // bigger than our 2nd best
    wf->entries[2] = new;
    wf->entries[1] = new;
  } else if (v >= wf->entries[2].v) {
    // bigger than our 3rd best
    wf->entries[2] = new;
  }

  return win_filter_apply_common(wf, win, t, v);
}

uint32_t
win_filter_apply_min (win_filter_t *wf, uint32_t win, uint64_t t, uint32_t v) {
  //   new minimum           || nothing in window
  if (v <= wf->entries[0].v || t - wf->entries[2].t > win) {
    return win_filter_reset(wf, t, v);
  }

  win_filter_entry_t new = {.t = t, .v = v};

  if (v <= wf->entries[1].v) {
    // smaller than our 2nd best
    wf->entries[2] = new;
    wf->entries[1] = new;

  } else if (v <= wf->entries[2].v) {
    // smaller than our 3rd best
    wf->entries[2] = new;
  }
  return win_filter_apply_common(wf, win, t, v);
}

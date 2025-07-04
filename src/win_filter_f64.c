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

// windowed min/max filter, see win_filter.c for explanation

#include "win_filter_f64.h"

static uint32_t
win_filter_f64_apply_common (win_filter_f64_t *wf, uint32_t win, uint64_t t, double v) {
  uint32_t dt = t - wf->entries[0].t;

  win_filter_f64_entry_t new = {.t = t, .v = v};

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
win_filter_f64_reset (win_filter_f64_t *wf, uint64_t t, double v) {
  win_filter_f64_entry_t val = {.t = t, .v = v};

  wf->entries[2] = wf->entries[1] = wf->entries[0] = val;
  return wf->entries[0].v;
}

uint32_t
win_filter_f64_apply_max (win_filter_f64_t *wf, uint32_t win, uint64_t t, double v) {
  //  new maximum           || nothing in window
  if (v >= wf->entries[0].v || t - wf->entries[2].t > win) {
    return win_filter_f64_reset(wf, t, v);
  }

  win_filter_f64_entry_t new = {.t = t, .v = v};

  if (v >= wf->entries[1].v) {
    // bigger than our 2nd best
    wf->entries[2] = new;
    wf->entries[1] = new;
  } else if (v >= wf->entries[2].v) {
    // bigger than our 3rd best
    wf->entries[2] = new;
  }

  return win_filter_f64_apply_common(wf, win, t, v);
}

uint32_t
win_filter_f64_apply_min (win_filter_f64_t *wf, uint32_t win, uint64_t t, double v) {
  //   new minimum           || nothing in window
  if (v <= wf->entries[0].v || t - wf->entries[2].t > win) {
    return win_filter_f64_reset(wf, t, v);
  }

  win_filter_f64_entry_t new = {.t = t, .v = v};

  if (v <= wf->entries[1].v) {
    // smaller than our 2nd best
    wf->entries[2] = new;
    wf->entries[1] = new;

  } else if (v <= wf->entries[2].v) {
    // smaller than our 3rd best
    wf->entries[2] = new;
  }
  return win_filter_f64_apply_common(wf, win, t, v);
}
